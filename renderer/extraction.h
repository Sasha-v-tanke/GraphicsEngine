#pragma once

#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/renderer/render_world.h>

#include <engine/controller/frame_scheduler.h>

namespace NRenderer::NInternal {

[[nodiscard]] RenderWorld ExtractRenderWorld(const NEcs::World& world,
                                             const NEngine::NController::FrameStorage& storage);

} // namespace NRenderer::NInternal
