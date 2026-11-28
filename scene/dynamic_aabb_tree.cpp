#include "dynamic_aabb_tree.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "spatial_query_detail.h"

namespace NScene {
namespace {

using NSpatialQueryDetail::FrustumHits;
using NSpatialQueryDetail::GlobalBox;
using NSpatialQueryDetail::IsBox;
using NSpatialQueryDetail::IsCoordinate;
using NSpatialQueryDetail::IsVector;
using NSpatialQueryDetail::Key;
using NSpatialQueryDetail::Overlaps;
using NSpatialQueryDetail::RayHits;
using NSpatialQueryDetail::Require;
using NSpatialQueryDetail::Union;

constexpr std::size_t NO_NODE = std::numeric_limits<std::size_t>::max();

struct Node {
    SpatialAabb Fat = {};
    SpatialAabb Tight = {};
    SpatialIdentity Identity = {};
    std::size_t Parent = NO_NODE;
    std::size_t Left = NO_NODE;
    std::size_t Right = NO_NODE;
    int Height = 0;
};

bool Contains(SpatialAabb outer, SpatialAabb inner) {
    return outer.Min.X <= inner.Min.X && outer.Min.Y <= inner.Min.Y && outer.Min.Z <= inner.Min.Z &&
           outer.Max.X >= inner.Max.X && outer.Max.Y >= inner.Max.Y && outer.Max.Z >= inner.Max.Z;
}

SpatialAabb Inflate(SpatialAabb bounds, double margin) {
    const auto low = [margin](double value) { return std::max(-SpatialBounds::MAX_COORDINATE, value - margin); };
    const auto high = [margin](double value) { return std::min(SpatialBounds::MAX_COORDINATE, value + margin); };
    return {{low(bounds.Min.X), low(bounds.Min.Y), low(bounds.Min.Z)},
            {high(bounds.Max.X), high(bounds.Max.Y), high(bounds.Max.Z)}};
}

double Area(SpatialAabb bounds) {
    const double x = bounds.Max.X - bounds.Min.X;
    const double y = bounds.Max.Y - bounds.Min.Y;
    const double z = bounds.Max.Z - bounds.Min.Z;
    return x * y + y * z + z * x;
}

bool IsLeaf(const Node& node) {
    return node.Left == NO_NODE;
}

// Indices survive vector reallocations. Released nodes are linked through Parent;
// copying Data preserves the free list, so churn cannot grow storage without bound.
std::size_t Allocate(auto& data, Node node) {
    if (data.Free == NO_NODE) {
        data.Nodes.push_back(node);
        return data.Nodes.size() - 1;
    }
    const auto index = data.Free;
    data.Free = data.Nodes[index].Parent;
    data.Nodes[index] = node;
    return index;
}

void Release(auto& data, std::size_t index) {
    data.Nodes[index] = {};
    data.Nodes[index].Parent = data.Free;
    data.Free = index;
}

void ReplaceChild(auto& data, std::size_t parent, std::size_t previous, std::size_t replacement) {
    if (parent == NO_NODE) {
        data.Root = replacement;
    } else if (data.Nodes[parent].Left == previous) {
        data.Nodes[parent].Left = replacement;
    } else {
        data.Nodes[parent].Right = replacement;
    }
    data.Nodes[replacement].Parent = parent;
}

void Refit(auto& data, std::size_t index) {
    auto& node = data.Nodes[index];
    const auto& left = data.Nodes[node.Left];
    const auto& right = data.Nodes[node.Right];
    node.Fat = Union(left.Fat, right.Fat);
    node.Height = 1 + std::max(left.Height, right.Height);
}

// AVL rotations keep traversal depth logarithmic even for ordered input and
// adversarial motion. Internal boxes are refit bottom-up after every rotation.
std::size_t Rotate(auto& data, std::size_t index, bool promoteRight) {
    auto& node = data.Nodes[index];
    const auto promoted = promoteRight ? node.Right : node.Left;
    auto& child = data.Nodes[promoted];
    const auto inner = promoteRight ? child.Left : child.Right;
    const auto parent = node.Parent;
    ReplaceChild(data, parent, index, promoted);
    if (promoteRight) {
        node.Right = inner;
        child.Left = index;
    } else {
        node.Left = inner;
        child.Right = index;
    }
    data.Nodes[inner].Parent = index;
    node.Parent = promoted;
    Refit(data, index);
    Refit(data, promoted);
    ++data.Statistics.Rotations;
    return promoted;
}

std::size_t Balance(auto& data, std::size_t index) {
    const auto left = data.Nodes[index].Left;
    const auto right = data.Nodes[index].Right;
    const int difference = data.Nodes[right].Height - data.Nodes[left].Height;
    if (difference > 1) {
        const auto& child = data.Nodes[right];
        if (data.Nodes[child.Left].Height > data.Nodes[child.Right].Height) {
            (void)Rotate(data, right, false);
        }
        return Rotate(data, index, true);
    }
    if (difference < -1) {
        const auto& child = data.Nodes[left];
        if (data.Nodes[child.Right].Height > data.Nodes[child.Left].Height) {
            (void)Rotate(data, left, true);
        }
        return Rotate(data, index, false);
    }
    return index;
}

void Repair(auto& data, std::size_t index) {
    while (index != NO_NODE) {
        Refit(data, index);
        index = data.Nodes[Balance(data, index)].Parent;
    }
}

std::size_t Insert(auto& data, SpatialIdentity identity, SpatialAabb tight) {
    const auto fat = Inflate(tight, data.Config.FatMargin);
    const auto leaf = Allocate(data, {.Fat = fat, .Tight = tight, .Identity = identity});
    if (data.Root == NO_NODE) {
        data.Root = leaf;
        return leaf;
    }
    auto sibling = data.Root;
    while (!IsLeaf(data.Nodes[sibling])) {
        const auto& node = data.Nodes[sibling];
        const auto& left = data.Nodes[node.Left];
        const auto& right = data.Nodes[node.Right];
        const double leftCost = Area(Union(left.Fat, fat)) - Area(left.Fat);
        const double rightCost = Area(Union(right.Fat, fat)) - Area(right.Fat);
        sibling = leftCost < rightCost || (leftCost == rightCost && left.Height <= right.Height) ? node.Left
                                                                                               : node.Right;
    }
    const auto previousParent = data.Nodes[sibling].Parent;
    const auto parent = Allocate(data,
                                 {.Fat = Union(data.Nodes[sibling].Fat, fat),
                                  .Parent = previousParent,
                                  .Left = sibling,
                                  .Right = leaf,
                                  .Height = 1});
    ReplaceChild(data, previousParent, sibling, parent);
    data.Nodes[sibling].Parent = parent;
    data.Nodes[leaf].Parent = parent;
    Repair(data, parent);
    return leaf;
}

void Remove(auto& data, std::size_t leaf) {
    const auto parent = data.Nodes[leaf].Parent;
    if (parent == NO_NODE) {
        data.Root = NO_NODE;
        Release(data, leaf);
        return;
    }
    const auto& node = data.Nodes[parent];
    const auto sibling = node.Left == leaf ? node.Right : node.Left;
    const auto grandparent = node.Parent;
    ReplaceChild(data, grandparent, parent, sibling);
    Release(data, leaf);
    Release(data, parent);
    Repair(data, grandparent);
}

SpatialQueryResult Query(const auto& data, const auto& matches) {
    SpatialQueryResult result;
    result.Version = data.Statistics.Version;
    if (data.Root == NO_NODE) {
        return result;
    }
    std::vector<std::size_t> stack;
    stack.reserve(data.Statistics.MaxDepth);
    stack.push_back(data.Root);
    while (!stack.empty()) {
        const auto index = stack.back();
        stack.pop_back();
        const auto& node = data.Nodes[index];
        ++result.Statistics.VisitedNodes;
        if (!matches(node.Fat)) {
            continue;
        }
        if (IsLeaf(node)) {
            ++result.Statistics.TestedItems;
            if (matches(node.Tight)) {
                result.Candidates.push_back(node.Identity);
            }
        } else {
            stack.push_back(node.Right);
            stack.push_back(node.Left);
        }
    }
    std::ranges::sort(result.Candidates,
                      [](SpatialIdentity left, SpatialIdentity right) { return Key(left) < Key(right); });
    result.Statistics.MatchedItems = result.Candidates.size();
    return result;
}

} // namespace

struct DynamicAabbTree::Data {
    SceneHandle Scene;
    DynamicAabbTreeConfig Config;
    DynamicAabbTreeStatistics Statistics;
    std::uint64_t WorldId = 0;
    std::size_t Root = NO_NODE;
    std::size_t Free = NO_NODE;
    std::vector<Node> Nodes;
    std::map<std::uint32_t, std::size_t> Leaves;
};

DynamicAabbTree::DynamicAabbTree(std::shared_ptr<const Data> data)
    : m_data(std::move(data)) {
}

DynamicAabbTree DynamicAabbTree::Build(SceneHandle scene,
                                       std::uint64_t version,
                                       std::span<const SpatialRecord> records,
                                       DynamicAabbTreeConfig config) {
    Require(scene.IsValid() && config.MaxRecords != 0 &&
                    config.MaxRecords <= std::numeric_limits<std::uint32_t>::max() / 2 &&
                    IsCoordinate(config.FatMargin) && config.FatMargin >= 0,
            "Invalid dynamic AABB tree configuration");
    auto data = std::make_shared<Data>();
    data->Scene = scene;
    data->Config = config;
    return DynamicAabbTree{std::move(data)}.Refresh(version, records);
}

DynamicAabbTree DynamicAabbTree::Refresh(std::uint64_t version, std::span<const SpatialRecord> records) const {
    Require(version > GetVersion() && records.size() <= m_data->Config.MaxRecords,
            "Invalid dynamic AABB tree version or input budget");
    // All admission uses copied values supplied by the committed record producer.
    // The map below borrows records only during this call, never in the snapshot.
    std::vector<const SpatialRecord*> incoming;
    incoming.reserve(records.size());
    const auto slotOf = [](const SpatialRecord* record) { return record->Identity.Entity.Identity.Index; };
    auto worldId = m_data->WorldId;
    for (const auto& record: records) {
        if (record.Validity != ESpatialValidity::VALID || record.Mobility != ESpatialMobility::DYNAMIC) {
            continue;
        }
        Require(record.Identity.Scene == m_data->Scene && record.Identity.Entity.WorldId != 0 &&
                        record.Identity.Entity.Identity.IsValid() && record.SourceRevision != 0 && record.Bounds &&
                        IsBox(record.Bounds->Aabb),
                "Malformed dynamic spatial record");
        Require(worldId == 0 || worldId == record.Identity.Entity.WorldId, "Mixed Worlds in dynamic AABB tree");
        worldId = record.Identity.Entity.WorldId;
        incoming.push_back(&record);
    }
    // Captured records normally arrive in entity order. Normalize arbitrary input
    // once, then merge both sorted sequences rather than looking up every record.
    if (!std::ranges::is_sorted(incoming, {}, slotOf)) {
        std::ranges::sort(incoming, {}, slotOf);
    }
    for (std::size_t index = 1; index < incoming.size(); ++index) {
        Require(slotOf(incoming[index - 1]) != slotOf(incoming[index]), "Duplicate dynamic spatial slot");
    }

    auto data = std::make_shared<Data>(*m_data);
    data->WorldId = worldId;
    data->Statistics = {};
    data->Statistics.Version = version;
    auto current = incoming.begin();
    for (auto it = data->Leaves.begin(); it != data->Leaves.end();) {
        while (current != incoming.end() && slotOf(*current) < it->first) {
            ++current;
        }
        if (current == incoming.end() || slotOf(*current) != it->first ||
            data->Nodes[it->second].Identity != (*current)->Identity) {
            Remove(*data, it->second);
            it = data->Leaves.erase(it);
            ++data->Statistics.Removed;
        } else {
            ++it;
        }
    }
    auto found = data->Leaves.begin();
    for (const auto* record: incoming) {
        const auto slot = slotOf(record);
        const auto tight = record->Bounds->Aabb;
        while (found != data->Leaves.end() && found->first < slot) {
            ++found;
        }
        if (found == data->Leaves.end() || found->first != slot) {
            data->Leaves.emplace_hint(found, slot, Insert(*data, record->Identity, tight));
            ++data->Statistics.Inserted;
        } else if (!Contains(data->Nodes[found->second].Fat, tight)) {
            Remove(*data, found->second);
            found->second = Insert(*data, record->Identity, tight);
            ++data->Statistics.Reinserted;
        } else if (data->Nodes[found->second].Tight != tight) {
            data->Nodes[found->second].Tight = tight;
            ++data->Statistics.UpdatedInFatBounds;
        }
    }
    data->Statistics.IndexedRecords = data->Leaves.size();
    data->Statistics.SkippedRecords = records.size() - data->Leaves.size();
    data->Statistics.MaxDepth =
            data->Root == NO_NODE ? 0 : static_cast<std::size_t>(data->Nodes[data->Root].Height) + 1;
    data->Statistics.AllocatedNodes = data->Nodes.size();
    return DynamicAabbTree{std::move(data)};
}

SceneHandle DynamicAabbTree::GetScene() const noexcept {
    return m_data->Scene;
}

std::uint64_t DynamicAabbTree::GetVersion() const noexcept {
    return m_data->Statistics.Version;
}

const DynamicAabbTreeStatistics& DynamicAabbTree::GetStatistics() const noexcept {
    return m_data->Statistics;
}

SpatialQueryResult DynamicAabbTree::QueryOverlap(SpatialAabb bounds, SpatialVector origin) const {
    Require(IsBox(bounds) && IsVector(origin), "Invalid dynamic AABB tree overlap query");
    const auto global = GlobalBox(bounds, origin);
    return Query(*m_data, [&](SpatialAabb box) { return Overlaps(box, global); });
}

SpatialQueryResult DynamicAabbTree::QueryRay(SpatialRay ray, SpatialVector origin) const {
    Require(IsVector(ray.Origin) && IsVector(ray.Direction) && IsVector(origin) && ray.Direction != SpatialVector{} &&
                    std::isfinite(ray.MinDistance) && ray.MinDistance >= 0 && !std::isnan(ray.MaxDistance) &&
                    ray.MaxDistance >= ray.MinDistance,
            "Invalid dynamic AABB tree ray query");
    return Query(*m_data, [&](SpatialAabb box) { return RayHits(box, ray, origin); });
}

SpatialQueryResult DynamicAabbTree::QueryFrustum(const SpatialFrustum& frustum, SpatialVector origin) const {
    Require(IsVector(origin), "Invalid dynamic AABB tree query origin");
    for (const SpatialPlane& plane: frustum.Planes) {
        Require(IsVector(plane.Normal) && plane.Normal != SpatialVector{} && IsCoordinate(plane.Offset),
                "Invalid dynamic AABB tree frustum plane");
    }
    return Query(*m_data, [&](SpatialAabb box) { return FrustumHits(box, frustum, origin); });
}

} // namespace NScene
