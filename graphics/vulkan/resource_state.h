#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>
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

struct VulkanBufferState {
    VkPipelineStageFlags2 StageMask = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 AccessMask = 0;
    std::uint32_t QueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
};

struct VulkanBufferTransition {
    VkBuffer Buffer = VK_NULL_HANDLE;
    VkDeviceSize Offset = 0;
    VkDeviceSize Size = VK_WHOLE_SIZE;
    EVulkanResourceUsage Usage = EVulkanResourceUsage::Undefined;
    std::uint32_t QueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
};

[[nodiscard]] VulkanBufferState MakeVulkanBufferState(EVulkanResourceUsage usage,
                                                      std::uint32_t queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED);

class VulkanResourceStateTracker final: public NCommon::NonTransferable {
public:
    void ImportImage(VkImage image, VkImageSubresourceRange subresourceRange, VulkanImageState state);

    void ForgetImage(VkImage image) noexcept;

    [[nodiscard]] std::optional<VulkanImageState> GetImageState(VkImage image,
                                                                VkImageSubresourceRange subresourceRange) const;

    [[nodiscard]] std::optional<VkImageMemoryBarrier2> TransitionImage(const VulkanImageTransition& transition);

    void ImportBuffer(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VulkanBufferState state);

    void ForgetBuffer(VkBuffer buffer) noexcept;

    [[nodiscard]] std::optional<VulkanBufferState>
    GetBufferState(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) const;

    [[nodiscard]] std::optional<VkBufferMemoryBarrier2> TransitionBuffer(const VulkanBufferTransition& transition);

    struct BufferRangeState {
        VkBuffer Buffer = VK_NULL_HANDLE;
        VkDeviceSize Offset = 0;
        VkDeviceSize Size = VK_WHOLE_SIZE;
        VulkanBufferState State;
    };

private:
    struct ImageKey {
        VkImage Image = VK_NULL_HANDLE;
        VkImageAspectFlags AspectMask = 0;
        std::uint32_t MipLevel = 0;
        std::uint32_t ArrayLayer = 0;

        [[nodiscard]] friend bool operator==(ImageKey left, ImageKey right) noexcept = default;
    };

    struct ImageKeyHash {
        [[nodiscard]] std::size_t operator()(ImageKey key) const noexcept;
    };

    [[nodiscard]] static ImageKey MakeImageKey(VkImage image, VkImageSubresourceRange subresourceRange);
    [[nodiscard]] static std::vector<ImageKey> MakeImageKeys(VkImage image, VkImageSubresourceRange subresourceRange);

    std::unordered_map<ImageKey, VulkanImageState, ImageKeyHash> m_images;
    std::vector<BufferRangeState> m_buffers;
};

} // namespace NVulkan
