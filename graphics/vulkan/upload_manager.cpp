#include "upload_manager.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

namespace {

std::string_view GetVkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

void ValidateUploadRange(const VulkanBufferAllocation& destination,
                         std::span<const std::byte> bytes,
                         std::uint64_t destinationOffsetBytes) {
    if (!destination.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan upload destination buffer is invalid");
    }

    if ((destination.GetUsage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan upload destination buffer requires transfer-destination usage");
    }

    if (bytes.empty()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan buffer upload payload must be non-empty");
    }

    const std::uint64_t sizeBytes = static_cast<std::uint64_t>(bytes.size());
    if (destinationOffsetBytes > destination.GetSizeBytes() ||
        sizeBytes > destination.GetSizeBytes() - destinationOffsetBytes) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan buffer upload range is out of bounds");
    }
}

[[nodiscard]] std::uint32_t GetBytesPerPixel(NGraphics::EImageFormat format) {
    switch (format) {
    case NGraphics::EImageFormat::R8_UNORM:
        return 1;
    case NGraphics::EImageFormat::RG8_UNORM:
        return 2;
    case NGraphics::EImageFormat::RGBA8_UNORM:
    case NGraphics::EImageFormat::BGRA8_UNORM:
        return 4;
    case NGraphics::EImageFormat::D32_FLOAT:
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Vulkan upload does not support depth images");
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Vulkan upload does not support image format");
}

[[nodiscard]] VkImageAspectFlags GetImageAspect(NGraphics::EImageFormat format) {
    if (format == NGraphics::EImageFormat::D32_FLOAT) {
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    return VK_IMAGE_ASPECT_COLOR_BIT;
}

[[nodiscard]] std::uint64_t GetUploadSizeBytes(const NGraphics::ImageDescriptor& descriptor) {
    const std::uint64_t bytesPerPixel = GetBytesPerPixel(descriptor.Format);
    const std::uint64_t width = descriptor.Extent.Width;
    const std::uint64_t height = descriptor.Extent.Height;

    if (width == 0 || height == 0 || descriptor.Extent.Depth != 1 || descriptor.MipLevels != 1 ||
        descriptor.ArrayLayers != 1 || width > std::numeric_limits<std::uint64_t>::max() / height ||
        width * height > std::numeric_limits<std::uint64_t>::max() / bytesPerPixel) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan upload requires a non-empty single-mip, single-layer 2D image");
    }

    return width * height * bytesPerPixel;
}

void ValidateImageUpload(const VulkanImageAllocation& destination,
                         std::span<const std::byte> bytes,
                         VkImageLayout finalLayout) {
    if (!destination.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan upload destination image is invalid");
    }

    if ((destination.GetUsage() & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan upload destination image requires transfer-destination usage");
    }

    if (finalLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && finalLayout != VK_IMAGE_LAYOUT_GENERAL) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan image upload final layout is unsupported");
    }

    if (finalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        (destination.GetUsage() & VK_IMAGE_USAGE_SAMPLED_BIT) == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan sampled image upload requires sampled usage");
    }

    const std::uint64_t expectedSize = GetUploadSizeBytes(destination.GetDescriptor());
    if (bytes.size() != expectedSize) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan image upload payload size does not match image extent");
    }
}

void TransitionImage(VkCommandBuffer commandBuffer,
                     VkImage image,
                     VkImageAspectFlags aspectMask,
                     VkImageLayout oldLayout,
                     VkPipelineStageFlags2 sourceStage,
                     VkAccessFlags2 sourceAccess,
                     VkImageLayout newLayout,
                     VkPipelineStageFlags2 destinationStage,
                     VkAccessFlags2 destinationAccess) {
    const VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = sourceStage,
            .srcAccessMask = sourceAccess,
            .dstStageMask = destinationStage,
            .dstAccessMask = destinationAccess,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange =
                    {
                            .aspectMask = aspectMask,
                            .baseMipLevel = 0,
                            .levelCount = 1,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                    },
    };
    const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = 0,
            .pMemoryBarriers = nullptr,
            .bufferMemoryBarrierCount = 0,
            .pBufferMemoryBarriers = nullptr,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
    };

    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void SynchronizeBufferUpload(VkCommandBuffer commandBuffer) {
    const VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    };
    const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier,
            .bufferMemoryBarrierCount = 0,
            .pBufferMemoryBarriers = nullptr,
            .imageMemoryBarrierCount = 0,
            .pImageMemoryBarriers = nullptr,
    };

    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void BeginCommandBuffer(VkCommandBuffer commandBuffer) {
    const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
    };

    const VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to begin Vulkan upload command buffer: {}",
                              GetVkResultName(result));
    }
}

void EndCommandBuffer(VkCommandBuffer commandBuffer) {
    const VkResult result = vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to end Vulkan upload command buffer: {}",
                              GetVkResultName(result));
    }
}

} // namespace

VulkanUploadManager::VulkanUploadManager(const VulkanDevice& device,
                                         VulkanMemoryAllocator& memoryAllocator,
                                         VulkanSubmissionManager& submissions)
    : m_device(device)
    , m_memoryAllocator(memoryAllocator)
    , m_submissions(submissions)
    , m_handle(device.GetHandle())
    , m_commandPool(CreateCommandPool()) {
    if (m_handle == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan upload manager requires a device");
    }
}

VulkanUploadManager::~VulkanUploadManager() {
    for (const PendingUpload& upload: m_pendingUploads) {
        m_submissions.Wait(upload.Completion, std::numeric_limits<std::uint64_t>::max());
    }

    ReleaseCompleted();

    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_handle, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
}

VulkanQueueCompletion VulkanUploadManager::UploadBuffer(VulkanBufferAllocation& destination,
                                                        std::span<const std::byte> bytes,
                                                        std::uint64_t destinationOffsetBytes) {
    ValidateUploadRange(destination, bytes, destinationOffsetBytes);
    ReleaseCompleted();

    VulkanBufferAllocation staging = CreateVulkanStagingBuffer(m_memoryAllocator, bytes.size());
    void* mapped = staging.Map();
    std::memcpy(mapped, bytes.data(), bytes.size());
    staging.Flush(0, bytes.size());
    staging.Unmap();

    const VkCommandBuffer commandBuffer = AllocateCommandBuffer();
    BeginCommandBuffer(commandBuffer);

    const VkBufferCopy copyRegion{
            .srcOffset = 0,
            .dstOffset = destinationOffsetBytes,
            .size = bytes.size(),
    };
    vkCmdCopyBuffer(commandBuffer, staging.GetBuffer(), destination.GetBuffer(), 1, &copyRegion);
    SynchronizeBufferUpload(commandBuffer);

    EndCommandBuffer(commandBuffer);

    const VkCommandBufferSubmitInfo commandBufferInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .pNext = nullptr,
            .commandBuffer = commandBuffer,
            .deviceMask = 0,
    };
    m_pendingUploads.reserve(m_pendingUploads.size() + 1);
    const VulkanQueueCompletion completion = m_submissions.SubmitGraphics(VulkanQueueSubmitInfo{
            .CommandBuffers = {commandBufferInfo},
    });

    m_pendingUploads.push_back(PendingUpload{
            .Completion = completion,
            .Staging = std::move(staging),
            .CommandBuffer = commandBuffer,
    });

    return completion;
}

VulkanQueueCompletion VulkanUploadManager::UploadImage(VulkanImageAllocation& destination,
                                                       std::span<const std::byte> bytes,
                                                       VkImageLayout finalLayout) {
    ValidateImageUpload(destination, bytes, finalLayout);
    ReleaseCompleted();

    VulkanBufferAllocation staging = CreateVulkanStagingBuffer(m_memoryAllocator, bytes.size());
    void* mapped = staging.Map();
    std::memcpy(mapped, bytes.data(), bytes.size());
    staging.Flush(0, bytes.size());
    staging.Unmap();

    const NGraphics::ImageDescriptor& descriptor = destination.GetDescriptor();
    const VkImageAspectFlags aspectMask = GetImageAspect(descriptor.Format);
    const VkCommandBuffer commandBuffer = AllocateCommandBuffer();
    BeginCommandBuffer(commandBuffer);
    TransitionImage(commandBuffer,
                    destination.GetImage(),
                    aspectMask,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_PIPELINE_STAGE_2_NONE,
                    VK_ACCESS_2_NONE,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const VkBufferImageCopy copyRegion{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource =
                    {
                            .aspectMask = aspectMask,
                            .mipLevel = 0,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                    },
            .imageOffset = {.x = 0, .y = 0, .z = 0},
            .imageExtent =
                    {
                            .width = descriptor.Extent.Width,
                            .height = descriptor.Extent.Height,
                            .depth = descriptor.Extent.Depth,
                    },
    };
    vkCmdCopyBufferToImage(commandBuffer,
                           staging.GetBuffer(),
                           destination.GetImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &copyRegion);
    TransitionImage(commandBuffer,
                    destination.GetImage(),
                    aspectMask,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    finalLayout,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    EndCommandBuffer(commandBuffer);

    const VkCommandBufferSubmitInfo commandBufferInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .pNext = nullptr,
            .commandBuffer = commandBuffer,
            .deviceMask = 0,
    };
    m_pendingUploads.reserve(m_pendingUploads.size() + 1);
    const VulkanQueueCompletion completion = m_submissions.SubmitGraphics(VulkanQueueSubmitInfo{
            .CommandBuffers = {commandBufferInfo},
    });

    m_pendingUploads.push_back(PendingUpload{
            .Completion = completion,
            .Staging = std::move(staging),
            .CommandBuffer = commandBuffer,
    });

    return completion;
}

void VulkanUploadManager::ReleaseCompleted() {
    auto upload = m_pendingUploads.begin();

    while (upload != m_pendingUploads.end()) {
        if (!m_submissions.IsCompleted(upload->Completion)) {
            ++upload;
            continue;
        }

        vkFreeCommandBuffers(m_handle, m_commandPool, 1, &upload->CommandBuffer);
        upload = m_pendingUploads.erase(upload);
    }
}

std::size_t VulkanUploadManager::GetPendingUploadCount() const noexcept {
    return m_pendingUploads.size();
}

VkCommandPool VulkanUploadManager::CreateCommandPool() const {
    const VulkanLockedQueue graphicsQueue = m_device.LockGraphicsQueue();
    const VkCommandPoolCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = graphicsQueue.GetFamilyIndex(),
    };

    VkCommandPool commandPool = VK_NULL_HANDLE;
    const VkResult result = vkCreateCommandPool(m_handle, &createInfo, nullptr, &commandPool);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to create Vulkan upload command pool: {}",
                              GetVkResultName(result));
    }

    return commandPool;
}

VkCommandBuffer VulkanUploadManager::AllocateCommandBuffer() const {
    const VkCommandBufferAllocateInfo allocateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = m_commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
    };

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    const VkResult result = vkAllocateCommandBuffers(m_handle, &allocateInfo, &commandBuffer);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to allocate Vulkan upload command buffer: {}",
                              GetVkResultName(result));
    }

    return commandBuffer;
}

} // namespace NVulkan
