#pragma once

#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/renderer/render_world.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <engine/controller/frame_scheduler.h>

namespace NRenderer::NInternal {

[[nodiscard]] const RenderWorld& ExtractRenderWorld(const NEcs::World& world,
                                                    const NResources::ResourceManager& resources,
                                                    const NEngine::NController::FrameStorage& storage);

} // namespace NRenderer::NInternal
