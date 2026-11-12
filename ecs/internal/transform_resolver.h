#pragma once

#include <memory>
#include <span>

#include <GraphicsEngine/ecs/hierarchy.h>
#include <GraphicsEngine/lib/common/change_tracker.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/lib/thread/task/task_system.h>

namespace NEcs::NInternal {

struct WorldTransformCache {
    EntityReference Entity;
    NMath::Mat4 Value = NMath::Mat4::Identity();
    NCommon::VersionStamp AppliedLocalRevision;
    NCommon::VersionStamp AppliedParentWorldRevision;
    EntityReference AppliedParent;
    NCommon::VersionStamp WorldRevision;
    bool Recomputed = false;
};

struct TransformResultData;

// Immutable owned values, including the requested ancestor closure. Does not pin World.
class ResolvedTransforms {
public:
    [[nodiscard]] const WorldTransformCache& Get(EntityReference entity) const;
    [[nodiscard]] std::size_t GetResolvedCount() const noexcept;
    [[nodiscard]] std::size_t GetRecomputedCount() const noexcept;

private:
    friend class TransformResolve;
    explicit ResolvedTransforms(std::shared_ptr<const TransformResultData> data);
    std::shared_ptr<const TransformResultData> m_data;
};

// TaskSystem must outlive scheduling, cancellation and observation. Wait Completion
// on the host; no worker waits. Failure/cancellation exposes no partial results.
class TransformResolve {
public:
    [[nodiscard]] NCommon::TaskHandle GetCompletion() const noexcept;
    [[nodiscard]] NCommon::TaskGroupHandle GetTraversal() const noexcept;
    [[nodiscard]] ResolvedTransforms ReadResolved(const NCommon::TaskSystem& tasks) const;

private:
    friend class TransformResolver;
    std::shared_ptr<const TransformResultData> m_data;
    NCommon::TaskHandle m_completion;
    NCommon::TaskGroupHandle m_traversal;
};

// One in-flight resolve per resolver. Topology leases provide exclusive access to
// local matrices/revisions; each traversal node owns its derived output exclusively.
class TransformResolver final: public NCommon::NonTransferable {
public:
    TransformResolver();
    [[nodiscard]] TransformResolve ScheduleResolve(TopologySnapshot topology,
                                                   std::span<const EntityReference> requiredEntities,
                                                   NCommon::TaskSystem& tasks);

private:
    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace NEcs::NInternal
