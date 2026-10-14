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

VkBufferMemoryBarrier2 MakeBufferBarrier(VkBuffer buffer,
                                         VkDeviceSize offset,
                                         VkDeviceSize size,
                                         VulkanBufferState source,
                                         VulkanBufferState destination) {
    return {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = source.StageMask,
            .srcAccessMask = source.AccessMask,
            .dstStageMask = destination.StageMask,
            .dstAccessMask = destination.AccessMask,
            .srcQueueFamilyIndex = source.QueueFamilyIndex,
            .dstQueueFamilyIndex = destination.QueueFamilyIndex,
            .buffer = buffer,
            .offset = offset,
            .size = size,
    };
}

bool HasWriteAccess(VkAccessFlags2 accessMask) noexcept {
    constexpr VkAccessFlags2 WRITE_ACCESS_MASK = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                                 VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

    return (accessMask & WRITE_ACCESS_MASK) != 0;
}

bool RequiresBarrier(VulkanImageState source, VulkanImageState destination) noexcept {
    return (HasWriteAccess(source.AccessMask) && HasWriteAccess(destination.AccessMask)) ||
           source.StageMask != destination.StageMask || source.AccessMask != destination.AccessMask ||
           source.Layout != destination.Layout || source.QueueFamilyIndex != destination.QueueFamilyIndex;
}

bool RequiresBarrier(VulkanBufferState source, VulkanBufferState destination) noexcept {
    return (HasWriteAccess(source.AccessMask) && HasWriteAccess(destination.AccessMask)) ||
           source.StageMask != destination.StageMask || source.AccessMask != destination.AccessMask ||
           source.QueueFamilyIndex != destination.QueueFamilyIndex;
}

template<class TState>
void ValidateInitialQueueFamily(TState state) {
    if (state.QueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan resource with explicit queue family requires imported initial state");
    }
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

VulkanBufferState MakeVulkanBufferState(EVulkanResourceUsage usage, std::uint32_t queueFamilyIndex) {
    switch (usage) {
    case EVulkanResourceUsage::Undefined:
    case EVulkanResourceUsage::Present:
        return {
                .StageMask = VK_PIPELINE_STAGE_2_NONE,
                .AccessMask = 0,
                .QueueFamilyIndex = queueFamilyIndex,
        };
    case EVulkanResourceUsage::ColorAttachmentWrite:
        return {
                .StageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .AccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .QueueFamilyIndex = queueFamilyIndex,
        };
    case EVulkanResourceUsage::TransferRead:
        return {
                .StageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .AccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .QueueFamilyIndex = queueFamilyIndex,
        };
    case EVulkanResourceUsage::TransferWrite:
        return {
                .StageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .AccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .QueueFamilyIndex = queueFamilyIndex,
        };
    case EVulkanResourceUsage::ShaderSampledRead:
        return {
                .StageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .AccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .QueueFamilyIndex = queueFamilyIndex,
        };
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unknown Vulkan buffer usage");
}

void VulkanResourceStateTracker::ImportImage(VkImage image,
                                             VkImageSubresourceRange subresourceRange,
                                             VulkanImageState state) {
    if (image == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan image state import requires an image");
    }

    for (const ImageKey key: MakeImageKeys(image, subresourceRange)) {
        m_images[key] = state;
    }
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
    std::optional<VulkanImageState> state;
    for (const ImageKey key: MakeImageKeys(image, subresourceRange)) {
        const auto it = m_images.find(key);
        if (it == m_images.end()) {
            return std::nullopt;
        }

        if (!state.has_value()) {
            state = it->second;
            continue;
        }

        if (state->StageMask != it->second.StageMask || state->AccessMask != it->second.AccessMask ||
            state->Layout != it->second.Layout || state->QueueFamilyIndex != it->second.QueueFamilyIndex) {
            return std::nullopt;
        }
    }

    return state;
}

std::optional<VkImageMemoryBarrier2>
VulkanResourceStateTracker::TransitionImage(const VulkanImageTransition& transition) {
    if (transition.Image == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan image transition requires an image");
    }

    const std::vector<ImageKey> keys = MakeImageKeys(transition.Image, transition.SubresourceRange);
    const VulkanImageState destination = MakeVulkanImageState(transition.Usage, transition.QueueFamilyIndex);
    std::optional<VulkanImageState> source;
    bool hasUntrackedSubresource = false;
    for (const ImageKey key: keys) {
        const auto it = m_images.find(key);
        if (it == m_images.end()) {
            hasUntrackedSubresource = true;
            continue;
        }

        if (!source.has_value()) {
            source = it->second;
            continue;
        }

        if (source->StageMask != it->second.StageMask || source->AccessMask != it->second.AccessMask ||
            source->Layout != it->second.Layout || source->QueueFamilyIndex != it->second.QueueFamilyIndex) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                  "Vulkan image transition range has mixed tracked states");
        }
    }

    if (hasUntrackedSubresource && source.has_value()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan image transition range is only partially tracked");
    }

    if (!source.has_value()) {
        ValidateInitialQueueFamily(destination);
        for (const ImageKey key: keys) {
            m_images.emplace(key, destination);
        }
        return MakeImageBarrier(transition.Image,
                                MakeVulkanImageState(EVulkanResourceUsage::Undefined),
                                destination,
                                transition.SubresourceRange);
    }

    for (const ImageKey key: keys) {
        m_images[key] = destination;
    }

    if (!RequiresBarrier(*source, destination)) {
        return std::nullopt;
    }

    return MakeImageBarrier(transition.Image, *source, destination, transition.SubresourceRange);
}

void VulkanResourceStateTracker::ImportBuffer(VkBuffer buffer,
                                              VkDeviceSize offset,
                                              VkDeviceSize size,
                                              VulkanBufferState state) {
    if (buffer == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan buffer state import requires a buffer");
    }

    m_buffers[{.Buffer = buffer, .Offset = offset, .Size = size}] = state;
}

void VulkanResourceStateTracker::ForgetBuffer(VkBuffer buffer) noexcept {
    for (auto it = m_buffers.begin(); it != m_buffers.end();) {
        if (it->first.Buffer == buffer) {
            it = m_buffers.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<VulkanBufferState>
VulkanResourceStateTracker::GetBufferState(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) const {
    const auto it = m_buffers.find({.Buffer = buffer, .Offset = offset, .Size = size});
    if (it == m_buffers.end()) {
        return std::nullopt;
    }

    return it->second;
}

std::optional<VkBufferMemoryBarrier2>
VulkanResourceStateTracker::TransitionBuffer(const VulkanBufferTransition& transition) {
    if (transition.Buffer == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan buffer transition requires a buffer");
    }

    const BufferKey key = {
            .Buffer = transition.Buffer,
            .Offset = transition.Offset,
            .Size = transition.Size,
    };
    const VulkanBufferState destination = MakeVulkanBufferState(transition.Usage, transition.QueueFamilyIndex);
    const auto it = m_buffers.find(key);
    if (it == m_buffers.end()) {
        ValidateInitialQueueFamily(destination);
        m_buffers.emplace(key, destination);
        return MakeBufferBarrier(transition.Buffer,
                                 transition.Offset,
                                 transition.Size,
                                 MakeVulkanBufferState(EVulkanResourceUsage::Undefined),
                                 destination);
    }

    const VulkanBufferState source = it->second;
    it->second = destination;

    if (!RequiresBarrier(source, destination)) {
        return std::nullopt;
    }

    return MakeBufferBarrier(transition.Buffer, transition.Offset, transition.Size, source, destination);
}

std::size_t VulkanResourceStateTracker::ImageKeyHash::operator()(ImageKey key) const noexcept {
    std::size_t hash = std::hash<VkImage>{}(key.Image);
    hash ^= std::hash<VkImageAspectFlags>{}(key.AspectMask) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
    hash ^= std::hash<std::uint32_t>{}(key.MipLevel) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
    hash ^= std::hash<std::uint32_t>{}(key.ArrayLayer) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
    return hash;
}

VulkanResourceStateTracker::ImageKey
VulkanResourceStateTracker::MakeImageKey(VkImage image, VkImageSubresourceRange subresourceRange) {
    return {
            .Image = image,
            .AspectMask = subresourceRange.aspectMask,
            .MipLevel = subresourceRange.baseMipLevel,
            .ArrayLayer = subresourceRange.baseArrayLayer,
    };
}

std::vector<VulkanResourceStateTracker::ImageKey>
VulkanResourceStateTracker::MakeImageKeys(VkImage image, VkImageSubresourceRange subresourceRange) {
    if (subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS ||
        subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS || subresourceRange.levelCount == 0 ||
        subresourceRange.layerCount == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan image state tracking requires explicit non-empty subresource ranges");
    }

    std::vector<ImageKey> keys;
    keys.reserve(subresourceRange.levelCount * subresourceRange.layerCount);
    for (std::uint32_t mip = 0; mip < subresourceRange.levelCount; ++mip) {
        for (std::uint32_t layer = 0; layer < subresourceRange.layerCount; ++layer) {
            keys.push_back({
                    .Image = image,
                    .AspectMask = subresourceRange.aspectMask,
                    .MipLevel = subresourceRange.baseMipLevel + mip,
                    .ArrayLayer = subresourceRange.baseArrayLayer + layer,
            });
        }
    }
    return keys;
}

std::size_t VulkanResourceStateTracker::BufferKeyHash::operator()(BufferKey key) const noexcept {
    std::size_t hash = std::hash<VkBuffer>{}(key.Buffer);
    hash ^= std::hash<VkDeviceSize>{}(key.Offset) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
    hash ^= std::hash<VkDeviceSize>{}(key.Size) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
    return hash;
}

} // namespace NVulkan
