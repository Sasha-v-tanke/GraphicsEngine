#include "transform_resolver.h"

#include <cmath>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <ecs/internal/adaptive_traversal.h>

namespace NEcs::NInternal {

struct TransformResultData {
    std::map<std::uint32_t, WorldTransformCache> Values;
    std::size_t Recomputed = 0;
};

struct TransformResolver::State {
    std::mutex Mutex;
    std::uint64_t WorldId = 0;
    bool Busy = false;
    std::map<std::uint32_t, WorldTransformCache> Cache;

    struct Admission {
        std::shared_ptr<State> Owner;
        ~Admission() {
            if (Owner) {
                const std::lock_guard lock{Owner->Mutex};
                Owner->Busy = false;
            }
        }
    };
};

ResolvedTransforms::ResolvedTransforms(std::shared_ptr<const TransformResultData> data)
    : m_data(std::move(data)) {
}
const WorldTransformCache& ResolvedTransforms::Get(EntityReference entity) const {
    const auto found = m_data->Values.find(entity.Identity.Index);
    if (found == m_data->Values.end() || found->second.Entity != entity) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Entity is outside the resolved transform closure");
    }
    return found->second;
}
std::size_t ResolvedTransforms::GetResolvedCount() const noexcept {
    return m_data->Values.size();
}
std::size_t ResolvedTransforms::GetRecomputedCount() const noexcept {
    return m_data->Recomputed;
}
ResolvedTransforms TransformResolve::ReadResolved(const NCommon::TaskSystem& tasks) const {
    if (!m_data || tasks.GetStatus(Completion) != NCommon::ETaskStatus::COMPLETED) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Transform resolve has not completed successfully");
    }
    return ResolvedTransforms{m_data};
}
TransformResolver::TransformResolver()
    : m_state(std::make_shared<State>()) {
}
TransformResolve TransformResolver::ScheduleResolve(TopologySnapshot topology,
                                                    std::span<const EntityReference> requiredEntities,
                                                    NCommon::TaskSystem& tasks) {
    auto admission = std::make_shared<State::Admission>();
    auto data = std::make_shared<TransformResultData>();
    std::vector<EntityReference> selected;
    std::vector<EntityReference> roots;
    {
        const std::lock_guard lock{m_state->Mutex};
        if (m_state->Busy || (m_state->WorldId != 0 && m_state->WorldId != topology.GetWorldId())) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Transform resolver is busy or belongs to another World");
        }
        m_state->WorldId = topology.GetWorldId();
        m_state->Busy = true;
        admission->Owner = m_state;
        // Only the requested closure is copied. Local edits never walk descendants.
        for (const auto required: requiredEntities) {
            (void)topology.GetLocalTransform(required);
            std::vector<EntityReference> path;
            for (auto entity = required; entity != EntityReference{}; entity = topology.GetParent(entity)) {
                (void)topology.GetLocalTransform(entity);
                if (data->Values.contains(entity.Identity.Index)) {
                    break;
                }
                path.push_back(entity);
            }
            for (const auto entity: std::views::reverse(path)) {
                const auto previous = m_state->Cache.find(entity.Identity.Index);
                WorldTransformCache value;
                value.Entity = entity;
                if (previous != m_state->Cache.end() && previous->second.Entity == entity) {
                    value = previous->second;
                }
                data->Values.emplace(entity.Identity.Index, value);
                selected.push_back(entity);
                if (topology.GetParent(entity) == EntityReference{}) {
                    roots.push_back(entity);
                }
            }
        }
    }
    const auto output = std::make_shared<TraversalOutput<WorldTransformCache>>();
    TraversalOperation<WorldTransformCache> operation{
            .Visit =
                    [topology, data, admission](TraversalInput<WorldTransformCache> input) {
                        const auto& local = TraversalTopologyAccess::Get(topology).Get(input.Entity).Local;
                        const auto parent = topology.GetParent(input.Entity);
                        const auto parentRevision =
                                input.ParentResult ? input.ParentResult->WorldRevision : NCommon::VersionStamp{};
                        auto value = std::as_const(data->Values).at(input.Entity.Identity.Index);
                        value.Recomputed = value.AppliedLocalRevision != local.LocalRevision ||
                                           value.AppliedParent != parent ||
                                           value.AppliedParentWorldRevision != parentRevision;
                        if (value.Recomputed) {
                            value.Value = input.ParentResult ? input.ParentResult->Value * local.Value : local.Value;
                            for (const float entry: value.Value.GetValues()) {
                                if (!std::isfinite(entry)) {
                                    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                                          "Resolved transform is not finite");
                                }
                            }
                            value.AppliedLocalRevision = local.LocalRevision;
                            value.AppliedParent = parent;
                            value.AppliedParentWorldRevision = parentRevision;
                            value.WorldRevision = NextTransformRevision(
                                    value.WorldRevision.IsValid() ? value.WorldRevision : NCommon::VersionStamp{1, 0});
                        }
                        return value;
                    },
            .Output = output,
            .SelectedNodes = selected};
    AdaptiveBatchPlanner planner{tasks};
    TransformResolve result;
    result.m_data = data;
    result.Traversal =
            planner.ScheduleTraversal(std::move(topology), roots, ETraversalMode::TOP_DOWN, std::move(operation));
    result.Completion =
            tasks.SubmitAfterGroup(result.Traversal, [data, output, admission, &tasks](NCommon::TaskContext&) {
                const auto resolved = output->TryAcquire(tasks);
                if (!resolved) {
                    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Transform traversal did not complete");
                }
                // All allocations precede publication. This second set of map nodes is
                // transferred without allocation, so failure cannot partially update Cache.
                std::map<std::uint32_t, WorldTransformCache> publication;
                for (auto& [index, value]: data->Values) {
                    value = resolved->Get(value.Entity);
                    data->Recomputed += static_cast<std::size_t>(value.Recomputed);
                    publication.emplace(index, value);
                }
                const std::lock_guard lock{admission->Owner->Mutex};
                while (!publication.empty()) {
                    auto node = publication.extract(publication.begin());
                    const auto existing = admission->Owner->Cache.find(node.key());
                    if (existing == admission->Owner->Cache.end()) {
                        admission->Owner->Cache.insert(std::move(node));
                    } else {
                        existing->second = node.mapped();
                    }
                }
            });
    return result;
}

} // namespace NEcs::NInternal
