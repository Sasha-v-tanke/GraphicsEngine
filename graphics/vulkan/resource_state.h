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
    void ImportImage(VkImage image, VkImageSubresourceRange subresourceRange, VulkanImageState state);

    void ForgetImage(VkImage image) noexcept;

    [[nodiscard]] std::optional<VulkanImageState> GetImageState(VkImage image,
                                                                VkImageSubresourceRange subresourceRange) const;

    [[nodiscard]] std::optional<VkImageMemoryBarrier2> TransitionImage(const VulkanImageTransition& transition);

private:
    struct ImageKey {
        VkImage Image = VK_NULL_HANDLE;
        VkImageAspectFlags AspectMask = 0;
        std::uint32_t BaseMipLevel = 0;
        std::uint32_t LevelCount = 0;
        std::uint32_t BaseArrayLayer = 0;
        std::uint32_t LayerCount = 0;

        [[nodiscard]] friend bool operator==(ImageKey left, ImageKey right) noexcept = default;
    };

    struct ImageKeyHash {
        [[nodiscard]] std::size_t operator()(ImageKey key) const noexcept;
    };

    [[nodiscard]] static ImageKey MakeImageKey(VkImage image, VkImageSubresourceRange subresourceRange);

private:
    std::unordered_map<ImageKey, VulkanImageState, ImageKeyHash> m_images;
};

} // namespace NVulkan
