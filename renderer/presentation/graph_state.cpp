#include "graph_state.h"

#include <lib/common/error/exception.h>

namespace NRenderer::NInternal {
NVulkan::EVulkanResourceUsage VulkanUsage(EResourceUsage usage, bool readsAttachment) {
    using NVulkan::EVulkanResourceUsage;
    switch (usage) {
    case EResourceUsage::UNDEFINED:
        return EVulkanResourceUsage::Undefined;
    case EResourceUsage::COLOR_ATTACHMENT_WRITE:
        return readsAttachment ? EVulkanResourceUsage::ColorAttachmentReadWrite
                               : EVulkanResourceUsage::ColorAttachmentWrite;
    case EResourceUsage::DEPTH_STENCIL_WRITE:
        return readsAttachment ? EVulkanResourceUsage::DepthStencilReadWrite : EVulkanResourceUsage::DepthStencilWrite;
    case EResourceUsage::SAMPLED_READ:
        return EVulkanResourceUsage::GraphicsSampledRead;
    case EResourceUsage::VERTEX_READ:
        return EVulkanResourceUsage::VertexRead;
    case EResourceUsage::INDEX_READ:
        return EVulkanResourceUsage::IndexRead;
    case EResourceUsage::UNIFORM_READ:
        return EVulkanResourceUsage::UniformRead;
    case EResourceUsage::TRANSFER_SOURCE:
        return EVulkanResourceUsage::TransferRead;
    case EResourceUsage::TRANSFER_DESTINATION:
        return EVulkanResourceUsage::TransferWrite;
    case EResourceUsage::PRESENT:
        return EVulkanResourceUsage::Present;
    }
    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unknown graph resource usage");
}
} // namespace NRenderer::NInternal
