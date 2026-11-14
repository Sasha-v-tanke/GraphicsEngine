#include "execution_validation.h"

#include <cmath>
#include <string_view>

#include <lib/common/error/exception.h>
#include <math/transform.h>

namespace NRenderer::NInternal {
void ValidateColorExecution(
        const CompiledGraph& graph,
        RenderFrameIdentity frame,
        const std::shared_ptr<const SwapchainGraphTarget>& target,
        const std::function<const NGraphics::GraphicsPipelineDescriptor&(const DrawCommandData&)>& resolve) {
    graph.ValidateResources(frame);
    const auto invalid = [](std::string_view message) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "{}", message);
    };
    if (!target || !target->IsValid() || !resolve || graph.GetPasses().empty()) {
        invalid("Graph requires the current acquired target and executable passes");
    }
    std::size_t targets = 0;
    for (const ImportedGraphResource& resource: graph.GetResources()) {
        if (resource.Kind == EGraphResourceKind::SWAPCHAIN) {
            if (resource.Swapchain.lock() != target || resource.InitialUsage != EResourceUsage::UNDEFINED) {
                invalid("Graph must use exactly the acquired swapchain target in Undefined state");
            }
            ++targets;
        } else if (resource.Lifetime != EGraphResourceLifetime::TRANSIENT ||
                   resource.Kind != EGraphResourceKind::IMAGE ||
                   resource.ImageDescription.Samples != NGraphics::ESampleCount::X1 ||
                   (resource.ImageDescription.Format != NGraphics::EPixelFormat::RGBA8_UNORM &&
                    resource.ImageDescription.Format != NGraphics::EPixelFormat::BGRA8_UNORM)) {
            invalid("Color executor supports single-sample RGBA8/BGRA8 transient images and the acquired target");
        }
    }
    if (targets != 1) {
        invalid("Graph must import exactly the acquired target");
    }
    for (const auto& pass: graph.GetPasses()) {
        const auto& desc = pass.Description;
        if (desc.Writes.size() != 1 || desc.ColorAttachments.size() != 1 || desc.DepthAttachment) {
            invalid("Color executor requires one color attachment per pass");
        }
        if (!desc.Reads.empty()) {
            invalid("Color executor does not bind graph resources to material inputs");
        }
        const GraphImageDescription& attachment = graph.GetResource(desc.ColorAttachments[0].Resource).ImageDescription;
        for (const auto& draw: desc.DrawCommands) {
            if (!draw.Mesh.IsValid() || !draw.Material.IsValid() || draw.MeshVersion == 0 ||
                draw.MaterialVersion == 0) {
                invalid("Invalid draw resource identity/version");
            }
            const auto matrix = draw.ViewProjection * NMath::ComposeTransform(draw.WorldTransform);
            for (float value: matrix.GetValues()) {
                if (!std::isfinite(value))
                    invalid("Nonfinite draw transform");
            }
            const auto& pipeline = resolve(draw);
            if (pipeline.ColorAttachmentFormats != std::vector{attachment.Format} ||
                pipeline.Samples != attachment.Samples || pipeline.DepthState.TestEnabled ||
                pipeline.DepthState.WriteEnabled ||
                pipeline.DepthAttachmentFormat != NGraphics::EPixelFormat::UNDEFINED) {
                invalid("Prepared draw pipeline is incompatible with the acquired color target");
            }
        }
    }
}
} // namespace NRenderer::NInternal
