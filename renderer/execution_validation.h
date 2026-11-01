#pragma once

#include <functional>

#include <GraphicsEngine/renderer/render_graph.h>

namespace NRenderer::NInternal {
// Validates the supported serial color subset before native recording. Resolver
// verifies exact prepared asset versions and returns their immutable pipeline data.
void ValidateColorExecution(
        const CompiledGraph& graph,
        RenderFrameIdentity frame,
        const std::shared_ptr<const SwapchainGraphTarget>& target,
        const std::function<const NGraphics::GraphicsPipelineDescriptor&(const DrawCommandData&)>& resolve);
} // namespace NRenderer::NInternal
