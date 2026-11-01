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
    if (!target || !target->IsValid() || !resolve || graph.GetResources().size() != 1 ||
        graph.GetResources()[0].Kind != EGraphResourceKind::SWAPCHAIN ||
        graph.GetResources()[0].Swapchain.lock() != target ||
        graph.GetResources()[0].InitialUsage != EResourceUsage::UNDEFINED || graph.GetPasses().empty()) {
        invalid("Graph must import exactly the current acquired target in Undefined state");
    }
    for (const auto& pass: graph.GetPasses()) {
        const auto& desc = pass.Description;
        if (!desc.Reads.empty() || desc.Writes.size() != 1 || desc.ColorAttachments.size() != 1 ||
            desc.DepthAttachment || desc.ColorAttachments[0].Resource != graph.GetResources()[0].Id) {
            invalid("Unsupported graph pass resources; R3 requires one acquired color attachment");
        }
        for (const auto& draw: desc.DrawCommands) {
            if (!draw.Mesh.IsValid() || !draw.Material.IsValid() || draw.MeshVersion == 0 ||
                draw.MaterialVersion == 0) {
                invalid("Invalid draw resource identity/version");
            }
            const auto matrix = NMath::ComposeTransform(draw.WorldTransform);
            for (float value: matrix.GetValues()) {
                if (!std::isfinite(value))
                    invalid("Nonfinite draw transform");
            }
            const auto& pipeline = resolve(draw);
            if (pipeline.ColorAttachmentFormats != std::vector{target->GetDescription().Format} ||
                pipeline.Samples != target->GetDescription().Samples || pipeline.DepthState.TestEnabled ||
                pipeline.DepthState.WriteEnabled ||
                pipeline.DepthAttachmentFormat != NGraphics::EPixelFormat::UNDEFINED) {
                invalid("Prepared draw pipeline is incompatible with the acquired color target");
            }
        }
    }
}
} // namespace NRenderer::NInternal
