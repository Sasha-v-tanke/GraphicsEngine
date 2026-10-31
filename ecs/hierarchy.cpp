#include "hierarchy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <ecs/internal/topology.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NEcs {
namespace {

void ValidateAffine(const NMath::Mat4& matrix) {
    for (float value: matrix.GetValues()) {
        if (!std::isfinite(value)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Hierarchy transform must be finite");
        }
    }
    if (matrix(3, 0) != 0 || matrix(3, 1) != 0 || matrix(3, 2) != 0 || matrix(3, 3) != 1) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Hierarchy transform must be affine");
    }
}

NMath::Mat4 InverseAffine(const NMath::Mat4& matrix) {
    std::array<std::array<double, 8>, 4> rows{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            rows[row][column] = matrix(row, column);
        }
        rows[row][row + 4] = 1;
    }
    for (std::size_t column = 0; column < 4; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row) {
            if (std::abs(rows[row][column]) > std::abs(rows[pivot][column])) {
                pivot = row;
            }
        }
        if (rows[pivot][column] == 0 || !std::isfinite(rows[pivot][column])) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "KEEP_WORLD parent is singular");
        }
        std::swap(rows[pivot], rows[column]);
        const double divisor = rows[column][column];
        for (double& value: rows[column]) {
            value /= divisor;
        }
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column) {
                continue;
            }
            const double factor = rows[row][column];
            for (std::size_t entry = 0; entry < 8; ++entry) {
                rows[row][entry] -= factor * rows[column][entry];
            }
        }
    }
    NMath::Mat4 inverse = NMath::Mat4::Identity();
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            inverse(row, column) = static_cast<float>(rows[row][column + 4]);
        }
    }
    ValidateAffine(inverse);
    return inverse;
}

} // namespace
namespace NInternal {

TopologyState::TopologyState(std::uint64_t worldId)
    : WorldId(worldId) {
}

void TopologyState::ValidateEpoch() const {
    if (Epoch == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Topology epoch exhausted");
    }
}

void TopologyState::BeginWrite() {
    std::lock_guard lock{Mutex};
    if (Readers != 0 || (WriteDepth != 0 && Writer != std::this_thread::get_id())) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Topology is pinned or another writer is active");
    }
    ValidateEpoch();
    Writer = std::this_thread::get_id();
    ++WriteDepth;
}

void TopologyState::EndWrite() noexcept {
    std::lock_guard lock{Mutex};
    --WriteDepth;
}

TopologyLease::TopologyLease(std::shared_ptr<TopologyState> state)
    : State(std::move(state))
    , Epoch(0) {
    std::lock_guard lock{State->Mutex};
    if (State->WriteDepth != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Cannot acquire topology during structural publication");
    }
    ++State->Readers;
    Epoch = State->Epoch;
}

TopologyLease::~TopologyLease() {
    std::lock_guard lock{State->Mutex};
    --State->Readers;
}

bool TopologyState::IsAlive(EntityReference entity) const noexcept {
    return entity.WorldId == WorldId && entity.Identity.IsValid() && entity.Identity.Index < Nodes.size() &&
           Nodes[entity.Identity.Index].Identity == entity.Identity;
}

const TopologyNode& TopologyState::Get(EntityReference entity) const {
    if (!IsAlive(entity)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Hierarchy reference is stale or belongs to another World");
    }
    return Nodes[entity.Identity.Index];
}

void TopologyState::Prepare(std::uint32_t index) {
    ValidateEpoch();
    if (index >= Nodes.size()) {
        Nodes.resize(static_cast<std::size_t>(index) + 1);
    }
}

void TopologyState::Create(Entity entity) noexcept {
    Nodes[entity.Index] = TopologyNode{.Identity = entity};
    ++Epoch;
}

void TopologyState::Detach(EntityReference entity) noexcept {
    auto& node = Nodes[entity.Identity.Index];
    if (node.PreviousSibling != EntityReference{}) {
        Nodes[node.PreviousSibling.Identity.Index].NextSibling = node.NextSibling;
    } else if (node.Parent != EntityReference{}) {
        Nodes[node.Parent.Identity.Index].FirstChild = node.NextSibling;
    }
    if (node.NextSibling != EntityReference{}) {
        Nodes[node.NextSibling.Identity.Index].PreviousSibling = node.PreviousSibling;
    } else if (node.Parent != EntityReference{}) {
        Nodes[node.Parent.Identity.Index].LastChild = node.PreviousSibling;
    }
    node.Parent = {};
    node.NextSibling = {};
    node.PreviousSibling = {};
}

void TopologyState::Destroy(Entity entity) noexcept {
    const EntityReference reference{WorldId, entity};
    auto& node = Nodes[entity.Index];
    Detach(reference);
    while (node.FirstChild != EntityReference{}) {
        Detach(node.FirstChild);
    }
    node = {};
    ++Epoch;
}

NMath::Mat4 TopologyState::Resolve(EntityReference entity) const {
    NMath::Mat4 result = Get(entity).Local;
    for (EntityReference parent = Get(entity).Parent; parent != EntityReference{}; parent = Get(parent).Parent) {
        result = Get(parent).Local * result;
        ValidateAffine(result);
    }
    return result;
}

void TopologyState::SetLocal(EntityReference entity, const NMath::Mat4& local) {
    (void)Get(entity);
    ValidateAffine(local);
    Nodes[entity.Identity.Index].Local = local;
}

void TopologyState::Reparent(EntityReference child, EntityReference parent, EReparentMode mode) {
    (void)Get(child);
    if (parent != EntityReference{}) {
        (void)Get(parent);
    }
    for (EntityReference cursor = parent; cursor != EntityReference{}; cursor = Get(cursor).Parent) {
        if (cursor == child) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Hierarchy reparent would create a cycle");
        }
    }
    if (mode != EReparentMode::KEEP_LOCAL && mode != EReparentMode::KEEP_WORLD) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unknown hierarchy reparent mode");
    }
    if (Get(child).Parent == parent) {
        return;
    }
    ValidateEpoch();
    NMath::Mat4 local = Get(child).Local;
    if (mode == EReparentMode::KEEP_WORLD) {
        const NMath::Mat4 world = Resolve(child);
        if (parent == EntityReference{}) {
            local = world;
        } else {
            local = InverseAffine(Resolve(parent)) * world;
        }
    }
    ValidateAffine(local);
    Detach(child);
    auto& node = Nodes[child.Identity.Index];
    node.Local = local;
    node.Parent = parent;
    if (parent != EntityReference{}) {
        auto& parentNode = Nodes[parent.Identity.Index];
        node.PreviousSibling = parentNode.LastChild;
        if (parentNode.LastChild != EntityReference{}) {
            Nodes[parentNode.LastChild.Identity.Index].NextSibling = child;
        } else {
            parentNode.FirstChild = child;
        }
        parentNode.LastChild = child;
    }
    ++Epoch;
}

} // namespace NInternal

TopologySnapshot::TopologySnapshot(std::shared_ptr<const NInternal::TopologyLease> lease)
    : m_lease(std::move(lease)) {
}
std::uint64_t TopologySnapshot::GetEpoch() const noexcept {
    return m_lease->Epoch;
}
std::uint64_t TopologySnapshot::GetWorldId() const noexcept {
    return m_lease->State->WorldId;
}
bool TopologySnapshot::IsAlive(EntityReference entity) const noexcept {
    return m_lease->State->IsAlive(entity);
}
EntityReference TopologySnapshot::GetParent(EntityReference entity) const {
    return m_lease->State->Get(entity).Parent;
}
TopologySnapshot::Children TopologySnapshot::GetChildren(EntityReference entity) const {
    return Children{m_lease, m_lease->State->Get(entity).FirstChild};
}
bool TopologySnapshot::IsAncestor(EntityReference ancestor, EntityReference descendant) const {
    (void)m_lease->State->Get(ancestor);
    (void)m_lease->State->Get(descendant);
    for (EntityReference parent = GetParent(descendant); parent != EntityReference{}; parent = GetParent(parent)) {
        if (parent == ancestor) {
            return true;
        }
    }
    return false;
}
NMath::Mat4 TopologySnapshot::GetLocalTransform(EntityReference entity) const {
    return m_lease->State->Get(entity).Local;
}
NMath::Mat4 TopologySnapshot::GetWorldTransform(EntityReference entity) const {
    return m_lease->State->Resolve(entity);
}
TopologySnapshot::Children::Children(std::shared_ptr<const NInternal::TopologyLease> lease, EntityReference first)
    : m_lease(std::move(lease))
    , m_first(first) {
}
TopologySnapshot::Children::Iterator::Iterator(std::shared_ptr<const NInternal::TopologyLease> lease,
                                               EntityReference entity)
    : m_lease(std::move(lease))
    , m_entity(entity) {
}
EntityReference TopologySnapshot::Children::Iterator::operator*() const {
    (void)m_lease->State->Get(m_entity);
    return m_entity;
}
TopologySnapshot::Children::Iterator& TopologySnapshot::Children::Iterator::operator++() {
    m_entity = m_lease->State->Get(m_entity).NextSibling;
    return *this;
}
TopologySnapshot::Children::Iterator TopologySnapshot::Children::Iterator::operator++(int) {
    Iterator previous = *this;
    ++*this;
    return previous;
}
TopologySnapshot::Children::Iterator TopologySnapshot::Children::begin() const {
    return Iterator{m_lease, m_first};
}
TopologySnapshot::Children::Iterator TopologySnapshot::Children::end() const {
    return Iterator{m_lease, {}};
}

} // namespace NEcs
