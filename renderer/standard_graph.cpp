#include "standard_graph.h"

#include <utility>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>

namespace NRenderer {

CompiledGraph BuildStandardGraph(RenderFrameIdentity frame,
                                 const RenderPlan& plan,
                                 std::optional<RenderObjectId> camera,
                                 const std::shared_ptr<const SwapchainGraphTarget>& target,
                                 const StandardGraphOptions& options) {
    if (frame != plan.GetFrame() || (camera && !camera->IsValid())) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid standard graph frame or camera identity");
    }
    if (options.InitialColorUsage != EResourceUsage::UNDEFINED &&
        options.InitialColorUsage != EResourceUsage::PRESENT) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid standard graph initial color usage");
    }
    RenderGraphBuilder builder{frame};
    const auto color = builder.ImportSwapchain(target, options.InitialColorUsage);
    GraphicsPassDescription pass;
    pass.Writes.push_back({color, EResourceUsage::COLOR_ATTACHMENT_WRITE});
    pass.ColorAttachments.push_back({.Resource = color, .ClearColor = options.ClearColor});
    if (camera) {
        for (const DrawCommandData& command: plan.GetDrawCommands()) {
            if (command.ViewId == *camera && command.Mesh.IsValid() && command.MeshVersion != 0 &&
                command.Material.IsValid() && command.MaterialVersion != 0) {
                pass.DrawCommands.push_back(command);
            }
        }
    }
    std::optional<GraphResourceId> depth;
    if (!pass.DrawCommands.empty()) {
        auto description = target->GetDescription();
        description.Format = NGraphics::EPixelFormat::D32_FLOAT;
        description.Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::DepthStencilAttachment);
        depth = builder.CreateTransientImage(description);
        pass.Writes.push_back({*depth, EResourceUsage::DEPTH_STENCIL_WRITE});
        pass.DepthAttachment = GraphAttachment{.Resource = *depth, .Store = EAttachmentStore::DISCARD};
    }
    const auto opaque = builder.AddGraphicsPass(std::move(pass));
    if (options.Extend) {
        options.Extend(builder, {color, depth, opaque});
    }
    return builder.Compile();
}

} // namespace NRenderer
