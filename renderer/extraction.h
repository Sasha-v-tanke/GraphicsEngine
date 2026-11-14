#pragma once

#include <functional>
#include <memory>

#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <GraphicsEngine/renderer/render_world.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <engine/controller/frame_scheduler.h>

namespace NRenderer::NInternal {

[[nodiscard]] const RenderWorld& ExtractRenderWorld(const NEcs::World& world,
                                                    const NResources::ResourceManager& resources,
                                                    const NEngine::NController::FrameStorage& storage);

// The caller task gates all chunks. The returned join publishes the snapshot and invokes completion.
// Readiness and leases are captured before scheduling chunks; subsequent resource transitions
// affect future extractions only. World and Resources outlive the join; World stays read-only.
[[nodiscard]] NCommon::TaskHandle ScheduleRenderWorldExtraction(const NEcs::World& world,
                                                                const NResources::ResourceManager& resources,
                                                                const NEngine::NController::FrameStorage& storage,
                                                                NCommon::TaskContext& task,
                                                                std::function<void(const RenderWorld&)> completion,
                                                                std::size_t chunkSize = 256);

struct ExtractionStatistics {
    bool FullRebuild = false;
    std::size_t UpdatedObjects = 0;
    std::size_t UpdatedViews = 0;
    std::size_t ResourceObservations = 0;
    std::size_t RecomputedTransforms = 0;
};

// One ordered extraction stream, at most one pending call. World is quiescent at capture;
// topology is pinned through resolve. Output owns all data and leases through FrameStorage.
// Changing World resets the cache; old frames remain valid. Destroying this facade does not
// cancel scheduled work. TaskSystem/Resources and frame storage outlive returned completion.
class IncrementalExtraction final: public NCommon::NonTransferable {
public:
    IncrementalExtraction();
    ~IncrementalExtraction();
    [[nodiscard]] NCommon::TaskHandle Schedule(NEcs::World& world,
                                               const NResources::ResourceManager& resources,
                                               const NEngine::NController::FrameStorage& storage,
                                               NCommon::TaskSystem& tasks,
                                               NCommon::TaskContext& task,
                                               std::function<void(const RenderWorld&)> completion);
    // Observe after completion, or while idle; serialized with publication.
    [[nodiscard]] ExtractionStatistics GetStatistics() const;
    // Idle only. Releases cache ownership; existing frame snapshots keep their leases.
    void Reset();

private:
    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace NRenderer::NInternal
