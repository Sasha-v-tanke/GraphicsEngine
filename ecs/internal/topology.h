#pragma once

#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#include <ecs/hierarchy.h>

namespace NEcs::NInternal {

struct TopologyNode {
    Entity Identity = {};
    EntityReference Parent = {};
    EntityReference FirstChild = {};
    EntityReference LastChild = {};
    EntityReference PreviousSibling = {};
    EntityReference NextSibling = {};
    NMath::Mat4 Local = NMath::Mat4::Identity();
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
