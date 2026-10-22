#pragma once

#include <functional>

#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <GraphicsEngine/renderer/render_world.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <engine/controller/frame_scheduler.h>

namespace NRenderer::NInternal {

[[nodiscard]] const RenderWorld& ExtractRenderWorld(const NEcs::World& world,
                                                    const NResources::ResourceManager& resources,
                                                    const NEngine::NController::FrameStorage& storage);

// The caller task gates all chunks. The returned join publishes the snapshot and invokes completion.
// World and Resources must outlive the join; World stays read-only until it completes.
[[nodiscard]] NCommon::TaskHandle ScheduleRenderWorldExtraction(const NEcs::World& world,
                                                                const NResources::ResourceManager& resources,
                                                                const NEngine::NController::FrameStorage& storage,
                                                                NCommon::TaskContext& task,
                                                                std::function<void(const RenderWorld&)> completion,
                                                                std::size_t chunkSize = 256);

} // namespace NRenderer::NInternal
