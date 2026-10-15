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
                         EVulkanResourceUsage finalUsage) {
    if (!destination.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan upload destination image is invalid");
    }

    if ((destination.GetUsage() & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan upload destination image requires transfer-destination usage");
    }

    if (finalUsage != EVulkanResourceUsage::ShaderSampledRead ||
        (destination.GetUsage() & VK_IMAGE_USAGE_SAMPLED_BIT) == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan sampled image upload requires sampled usage");
    }

    const std::uint64_t expectedSize = GetUploadSizeBytes(destination.GetDescriptor());
    if (bytes.size() != expectedSize) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan image upload payload size does not match image extent");
    }
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
                                         VulkanSubmissionManager& submissions,
                                         VulkanResourceStateTracker& resourceStates)
    : m_device(device)
    , m_memoryAllocator(memoryAllocator)
    , m_submissions(submissions)
    , m_resourceStates(resourceStates)
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
    VulkanResourceStateTracker::Transaction stateTransaction = m_resourceStates.BeginTransaction();

    const VkBufferCopy copyRegion{
            .srcOffset = 0,
            .dstOffset = destinationOffsetBytes,
            .size = bytes.size(),
    };
    if (const std::optional<VkBufferMemoryBarrier2> barrier = m_resourceStates.TransitionBuffer({
                .Buffer = destination.GetBuffer(),
                .Offset = destinationOffsetBytes,
                .Size = bytes.size(),
                .Usage = EVulkanResourceUsage::TransferWrite,
        });
        barrier.has_value()) {
        RecordVulkanResourceBarrier(commandBuffer, *barrier);
    }
    vkCmdCopyBuffer(commandBuffer, staging.GetBuffer(), destination.GetBuffer(), 1, &copyRegion);

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
    stateTransaction.Commit();

    m_pendingUploads.push_back(PendingUpload{
            .Completion = completion,
            .Staging = std::move(staging),
            .CommandBuffer = commandBuffer,
    });

    return completion;
}

VulkanQueueCompletion VulkanUploadManager::UploadImage(VulkanImageAllocation& destination,
                                                       std::span<const std::byte> bytes,
                                                       EVulkanResourceUsage finalUsage) {
    ValidateImageUpload(destination, bytes, finalUsage);
    ReleaseCompleted();

    VulkanBufferAllocation staging = CreateVulkanStagingBuffer(m_memoryAllocator, bytes.size());
    void* mapped = staging.Map();
    std::memcpy(mapped, bytes.data(), bytes.size());
    staging.Flush(0, bytes.size());
    staging.Unmap();

    const NGraphics::ImageDescriptor& descriptor = destination.GetDescriptor();
    const VkImageAspectFlags aspectMask = GetImageAspect(descriptor.Format);
    const VkImageSubresourceRange subresourceRange{
            .aspectMask = aspectMask,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
    };
    const VkCommandBuffer commandBuffer = AllocateCommandBuffer();
    BeginCommandBuffer(commandBuffer);
    VulkanResourceStateTracker::Transaction stateTransaction = m_resourceStates.BeginTransaction();
    if (const std::optional<VkImageMemoryBarrier2> barrier = m_resourceStates.TransitionImage({
                .Image = destination.GetImage(),
                .Usage = EVulkanResourceUsage::TransferWrite,
                .SubresourceRange = subresourceRange,
        });
        barrier.has_value()) {
        RecordVulkanResourceBarrier(commandBuffer, *barrier);
    }

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
    if (const std::optional<VkImageMemoryBarrier2> barrier = m_resourceStates.TransitionImage({
                .Image = destination.GetImage(),
                .Usage = finalUsage,
                .SubresourceRange = subresourceRange,
        });
        barrier.has_value()) {
        RecordVulkanResourceBarrier(commandBuffer, *barrier);
    }
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
    stateTransaction.Commit();

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
