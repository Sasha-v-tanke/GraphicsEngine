#include "resource_state.h"

#include <cstddef>
#include <functional>
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
    constexpr VkAccessFlags2 WRITE_ACCESS_MASK = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                                 VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

    return ((source.AccessMask & destination.AccessMask & WRITE_ACCESS_MASK) != 0) ||
           source.StageMask != destination.StageMask || source.AccessMask != destination.AccessMask ||
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

void VulkanResourceStateTracker::ImportImage(VkImage image,
                                             VkImageSubresourceRange subresourceRange,
                                             VulkanImageState state) {
    if (image == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan image state import requires an image");
    }

    m_images[MakeImageKey(image, subresourceRange)] = state;
}

void VulkanResourceStateTracker::ForgetImage(VkImage image) noexcept {
    for (auto it = m_images.begin(); it != m_images.end();) {
        if (it->first.Image == image) {
            it = m_images.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<VulkanImageState>
VulkanResourceStateTracker::GetImageState(VkImage image, VkImageSubresourceRange subresourceRange) const {
    const auto it = m_images.find(MakeImageKey(image, subresourceRange));
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

    const ImageKey key = MakeImageKey(transition.Image, transition.SubresourceRange);
    const VulkanImageState destination = MakeVulkanImageState(transition.Usage, transition.QueueFamilyIndex);
    const auto it = m_images.find(key);
    if (it == m_images.end()) {
        m_images.emplace(key, destination);
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

std::size_t VulkanResourceStateTracker::ImageKeyHash::operator()(ImageKey key) const noexcept {
    std::size_t hash = std::hash<VkImage>{}(key.Image);
    hash ^= std::hash<VkImageAspectFlags>{}(key.AspectMask) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
    hash ^= std::hash<std::uint32_t>{}(key.BaseMipLevel) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
    hash ^= std::hash<std::uint32_t>{}(key.LevelCount) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
    hash ^= std::hash<std::uint32_t>{}(key.BaseArrayLayer) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
    hash ^= std::hash<std::uint32_t>{}(key.LayerCount) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
    return hash;
}

VulkanResourceStateTracker::ImageKey
VulkanResourceStateTracker::MakeImageKey(VkImage image, VkImageSubresourceRange subresourceRange) {
    return {
            .Image = image,
            .AspectMask = subresourceRange.aspectMask,
            .BaseMipLevel = subresourceRange.baseMipLevel,
            .LevelCount = subresourceRange.levelCount,
            .BaseArrayLayer = subresourceRange.baseArrayLayer,
            .LayerCount = subresourceRange.layerCount,
    };
}

} // namespace NVulkan
