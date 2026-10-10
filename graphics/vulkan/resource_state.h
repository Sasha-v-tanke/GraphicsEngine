#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vulkan.h>

#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NVulkan {

enum class EVulkanResourceUsage {
    Undefined,
    Present,
    ColorAttachmentWrite,
    TransferRead,
    TransferWrite,
    ShaderSampledRead,
};

struct VulkanImageState {
    VkPipelineStageFlags2 StageMask = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 AccessMask = 0;
    VkImageLayout Layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t QueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
};

struct VulkanImageTransition {
    VkImage Image = VK_NULL_HANDLE;
    EVulkanResourceUsage Usage = EVulkanResourceUsage::Undefined;
    VkImageSubresourceRange SubresourceRange{};
    std::uint32_t QueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
};

[[nodiscard]] VulkanImageState MakeVulkanImageState(EVulkanResourceUsage usage,
                                                    std::uint32_t queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED);

class VulkanResourceStateTracker final: public NCommon::NonTransferable {
public:
    void ImportImage(VkImage image, VulkanImageState state);

    void ForgetImage(VkImage image) noexcept;

    [[nodiscard]] std::optional<VulkanImageState> GetImageState(VkImage image) const;

    [[nodiscard]] std::optional<VkImageMemoryBarrier2> TransitionImage(const VulkanImageTransition& transition);

private:
    std::unordered_map<VkImage, VulkanImageState> m_images;
};

} // namespace NVulkan
