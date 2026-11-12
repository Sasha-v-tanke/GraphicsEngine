#pragma once

#include <cstddef>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <GraphicsEngine/lib/common/change_tracker.h>
#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>
#include <ecs/hierarchy.h>

namespace NEcs::NInternal {

// Versions are owned by the topology writer, never a shared per-write counter.
inline NCommon::VersionStamp NextTransformRevision(NCommon::VersionStamp revision) {
    if (revision.Value != std::numeric_limits<std::uint64_t>::max()) {
        ++revision.Value;
    } else {
        if (revision.Era == std::numeric_limits<std::uint64_t>::max()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Transform revision exhausted");
        }
        ++revision.Era;
        revision.Value = 0;
    }
    return revision;
}

struct LocalTransform {
    NMath::Mat4 Value = NMath::Mat4::Identity();
    NCommon::VersionStamp LocalRevision{1, 0};
};

struct TopologyNode {
    Entity Identity = {};
    EntityReference Parent = {};
    EntityReference FirstChild = {};
    EntityReference LastChild = {};
    EntityReference PreviousSibling = {};
    EntityReference NextSibling = {};
    LocalTransform Local = {};
};

struct TopologyState {
    explicit TopologyState(std::uint64_t worldId);
    void BeginWrite();
    void EndWrite() noexcept;
    void Prepare(std::uint32_t index);
    void Create(Entity entity) noexcept;
    void Destroy(Entity entity) noexcept;
    void Reparent(EntityReference child, EntityReference parent, EReparentMode mode);
    void SetLocal(EntityReference entity, const NMath::Mat4& local);
    [[nodiscard]] bool IsAlive(EntityReference entity) const noexcept;
    [[nodiscard]] const TopologyNode& Get(EntityReference entity) const;
    [[nodiscard]] NMath::Mat4 Resolve(EntityReference entity) const;
    void Detach(EntityReference entity) noexcept;
    void ValidateEpoch() const;

    const std::uint64_t WorldId;
    std::uint64_t Epoch = 0;
    std::vector<TopologyNode> Nodes;
    std::mutex Mutex;
    std::size_t Readers = 0;
    std::size_t WriteDepth = 0;
    std::thread::id Writer;
};

struct TopologyLease {
    explicit TopologyLease(std::shared_ptr<TopologyState> state);
    ~TopologyLease();
    TopologyLease(const TopologyLease&) = delete;
    TopologyLease& operator=(const TopologyLease&) = delete;
    std::shared_ptr<TopologyState> State;
    std::uint64_t Epoch;
};

} // namespace NEcs::NInternal
