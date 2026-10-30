#include "rendering_scope.h"

#include <cstdint>
#include <limits>
#include <vector>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

VulkanRenderingScope::VulkanRenderingScope(VkCommandBuffer commands,
                                           VkRect2D area,
                                           std::span<const VulkanColorAttachment> colors,
                                           ERenderingContents contents) {
    if (commands == VK_NULL_HANDLE || area.offset.x < 0 || area.offset.y < 0 || area.extent.width == 0 ||
        area.extent.height == 0 || colors.empty() || colors.size() > std::numeric_limits<std::uint32_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid dynamic rendering scope");
    }
    VkRenderingFlags flags = 0;
    switch (contents) {
    case ERenderingContents::INLINE:
        break;
    case ERenderingContents::SECONDARY_COMMAND_BUFFERS:
        flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
        break;
    default:
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid rendering contents");
    }
    // Validate every attachment before opening the native scope: an exception must leave recording outside it.
    std::vector<VkRenderingAttachmentInfo> attachments;
    attachments.reserve(colors.size());
    for (const auto& color: colors) {
        if (color.View == VK_NULL_HANDLE ||
            (color.Load != VK_ATTACHMENT_LOAD_OP_CLEAR && color.Load != VK_ATTACHMENT_LOAD_OP_LOAD &&
             color.Load != VK_ATTACHMENT_LOAD_OP_DONT_CARE) ||
            (color.Store != VK_ATTACHMENT_STORE_OP_STORE && color.Store != VK_ATTACHMENT_STORE_OP_DONT_CARE)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid dynamic color attachment");
        }
        VkRenderingAttachmentInfo attachment{};
        attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageView = color.View;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = color.Load;
        attachment.storeOp = color.Store;
        attachment.clearValue.color = color.Clear;
        attachments.push_back(attachment);
    }
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.flags = flags;
    rendering.renderArea = area;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(attachments.size());
    rendering.pColorAttachments = attachments.data();
    // Attachment descriptions are consumed by this command; only borrowed GPU views outlive the call.
    vkCmdBeginRendering(commands, &rendering);
    m_commands = commands;
}

VulkanRenderingScope::~VulkanRenderingScope() {
    vkCmdEndRendering(m_commands);
}
} // namespace NVulkan
