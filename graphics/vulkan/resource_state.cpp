#include "resource_state.h"

#include <utility>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

namespace {

VkImageMemoryBarrier2 MakeImageBarrier(VkImage image,
                                       VulkanImageState source,
                                       VulkanImageState destination,
                                       VkImageSubresourceRange subresourceRange) {
    return {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = source.StageMask,
            .srcAccessMask = source.AccessMask,
            .dstStageMask = destination.StageMask,
            .dstAccessMask = destination.AccessMask,
            .oldLayout = source.Layout,
            .newLayout = destination.Layout,
            .srcQueueFamilyIndex = source.QueueFamilyIndex,
            .dstQueueFamilyIndex = destination.QueueFamilyIndex,
            .image = image,
            .subresourceRange = subresourceRange,
    };
}

bool RequiresBarrier(VulkanImageState source, VulkanImageState destination) noexcept {
    return source.StageMask != destination.StageMask || source.AccessMask != destination.AccessMask ||
           source.Layout != destination.Layout || source.QueueFamilyIndex != destination.QueueFamilyIndex;
}

} // namespace

VulkanImageState MakeVulkanImageState(EVulkanResourceUsage usage, std::uint32_t queueFamilyIndex) {
    switch (usage) {
    case EVulkanResourceUsage::Undefined:
        return {
                .StageMask = VK_PIPELINE_STAGE_2_NONE,
                .AccessMask = 0,
                .Layout = VK_IMAGE_LAYOUT_UNDEFINED,
                .QueueFamilyIndex = queueFamilyIndex,
        };
    case EVulkanResourceUsage::Present:
        return {
                .StageMask = VK_PIPELINE_STAGE_2_NONE,
                .AccessMask = 0,
                .Layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .QueueFamilyIndex = queueFamilyIndex,
        };
    case EVulkanResourceUsage::ColorAttachmentWrite:
        return {
                .StageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .AccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .Layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .QueueFamilyIndex = queueFamilyIndex,
        };
    case EVulkanResourceUsage::TransferRead:
        return {
                .StageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .AccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .Layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .QueueFamilyIndex = queueFamilyIndex,
        };
    case EVulkanResourceUsage::TransferWrite:
        return {
                .StageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .AccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .Layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .QueueFamilyIndex = queueFamilyIndex,
        };
    case EVulkanResourceUsage::ShaderSampledRead:
        return {
                .StageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .AccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .QueueFamilyIndex = queueFamilyIndex,
        };
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unknown Vulkan image usage");
}

void VulkanResourceStateTracker::ImportImage(VkImage image, VulkanImageState state) {
    if (image == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan image state import requires an image");
    }

    m_images[image] = state;
}

void VulkanResourceStateTracker::ForgetImage(VkImage image) noexcept {
    m_images.erase(image);
}

std::optional<VulkanImageState> VulkanResourceStateTracker::GetImageState(VkImage image) const {
    const auto it = m_images.find(image);
    if (it == m_images.end()) {
        return std::nullopt;
    }

    return it->second;
}

std::optional<VkImageMemoryBarrier2>
VulkanResourceStateTracker::TransitionImage(const VulkanImageTransition& transition) {
    if (transition.Image == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan image transition requires an image");
    }

    const VulkanImageState destination = MakeVulkanImageState(transition.Usage, transition.QueueFamilyIndex);
    const auto it = m_images.find(transition.Image);
    if (it == m_images.end()) {
        m_images.emplace(transition.Image, destination);
        return MakeImageBarrier(transition.Image,
                                MakeVulkanImageState(EVulkanResourceUsage::Undefined),
                                destination,
                                transition.SubresourceRange);
    }

    const VulkanImageState source = it->second;
    it->second = destination;

    if (!RequiresBarrier(source, destination)) {
        return std::nullopt;
    }

    return MakeImageBarrier(transition.Image, source, destination, transition.SubresourceRange);
}

} // namespace NVulkan
