#pragma once

#include <span>
#include <vulkan.h>

#include <lib/common/wrapper/non_transferable.h>

namespace NVulkan {

// Backend-resolved attachments are borrowed for recording; their GPU lifetime belongs to the submission owner.
// Format/sample compatibility is described by GraphicsPipelineDescriptor, never by render-pass objects.
struct VulkanColorAttachment {
    VkImageView View = VK_NULL_HANDLE;
    VkAttachmentLoadOp Load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp Store = VK_ATTACHMENT_STORE_OP_STORE;
    VkClearColorValue Clear{};
};

enum class ERenderingContents {
    INLINE,
    SECONDARY_COMMAND_BUFFERS,
};

// The caller owns a recording primary command buffer outside any rendering scope and transitions attachments
// to COLOR_ATTACHMENT_OPTIMAL first. Scope end does not submit or transition images. Attachments must cover
// the render area, use one layer, and match the bound pipeline formats/sample count. Secondary buffers must
// use matching VkCommandBufferInheritanceRenderingInfo; they remain owned by the frame/worker contexts.
class VulkanRenderingScope final: public NCommon::NonTransferable {
public:
    VulkanRenderingScope(VkCommandBuffer commands,
                         VkRect2D area,
                         std::span<const VulkanColorAttachment> colors,
                         ERenderingContents contents = ERenderingContents::INLINE);
    ~VulkanRenderingScope();

private:
    VkCommandBuffer m_commands = VK_NULL_HANDLE;
};
} // namespace NVulkan
