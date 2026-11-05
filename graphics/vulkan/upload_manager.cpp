#include "upload_manager.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>

#include <graphics/vulkan/upload_layout.h>
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
                                         VulkanResourceStateTracker& resourceStates,
                                         VulkanUploadConfig config)
    : m_device(device)
    , m_memoryAllocator(memoryAllocator)
    , m_submissions(submissions)
    , m_resourceStates(resourceStates)
    , m_config(config)
    , m_handle(device.GetHandle()) {
    if (m_handle == VK_NULL_HANDLE || config.ArenaSizeBytes == 0 || config.ArenaSizeBytes > config.MaxStagingBytes ||
        config.MaxBatchUploads == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid Vulkan upload manager configuration");
    }
    m_commandPool = CreateCommandPool();
}

VulkanUploadManager::~VulkanUploadManager() noexcept {
    try {
        for (const auto& arena: m_arenas) {
            if (arena->Completion.Value != 0) {
                m_submissions.Wait(arena->Completion, std::numeric_limits<std::uint64_t>::max());
            }
        }
    } catch (...) {
        // A failed device follows backend teardown; steady-state recycling never waits.
    }
    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_handle, m_commandPool, nullptr);
    }
    m_arenas.clear();
}

VulkanQueueCompletion VulkanUploadManager::UploadBuffer(VulkanBufferAllocation& destination,
                                                        std::span<const std::byte> bytes,
                                                        std::uint64_t destinationOffsetBytes) {
    const VulkanBufferUpload upload{&destination, bytes, destinationOffsetBytes};
    return UploadBatch({&upload, 1});
}

VulkanQueueCompletion VulkanUploadManager::UploadImage(VulkanImageAllocation& destination,
                                                       std::span<const std::byte> bytes,
                                                       EVulkanResourceUsage finalUsage) {
    const VulkanImageUpload upload{&destination, bytes, finalUsage};
    return UploadBatch({}, {&upload, 1});
}

VulkanQueueCompletion VulkanUploadManager::UploadBatch(std::span<const VulkanBufferUpload> buffers,
                                                       std::span<const VulkanImageUpload> images) {
    if (buffers.size() > m_config.MaxBatchUploads || images.size() > m_config.MaxBatchUploads - buffers.size() ||
        (buffers.empty() && images.empty())) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid Vulkan upload batch count");
    }
    std::uint64_t sizeBytes = 0;
    std::uint64_t payloadBytes = 0;
    std::vector<std::uint64_t> offsets;
    offsets.reserve(buffers.size() + images.size());
    for (const VulkanBufferUpload& upload: buffers) {
        if (upload.Destination == nullptr) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Null Vulkan upload buffer");
        }
        ValidateUploadRange(*upload.Destination, upload.Bytes, upload.DestinationOffsetBytes);
        offsets.push_back(NInternal::AppendUploadSlice(sizeBytes, upload.Bytes.size()));
        payloadBytes += upload.Bytes.size();
    }
    for (const VulkanImageUpload& upload: images) {
        if (upload.Destination == nullptr) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Null Vulkan upload image");
        }
        ValidateImageUpload(*upload.Destination, upload.Bytes, upload.FinalUsage);
        offsets.push_back(NInternal::AppendUploadSlice(sizeBytes, upload.Bytes.size()));
        payloadBytes += upload.Bytes.size();
    }
    ReleaseCompleted();
    StagingArena& arena = AcquireArena(sizeBytes);
    arena.Destinations.clear();
    arena.Destinations.reserve(offsets.size());
    const VkResult resetResult = vkResetCommandBuffer(arena.CommandBuffer, 0);
    if (resetResult != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to reset Vulkan upload command buffer: {}",
                              GetVkResultName(resetResult));
    }
    try {
        BeginCommandBuffer(arena.CommandBuffer);
        VulkanResourceStateTracker::Transaction stateTransaction = m_resourceStates.BeginTransaction();
        std::size_t index = 0;
        for (const VulkanBufferUpload& upload: buffers) {
            const std::uint64_t offset = offsets[index++];
            std::memcpy(arena.Mapped + offset, upload.Bytes.data(), upload.Bytes.size());
            arena.Destinations.push_back(upload.Destination->GetRetentionToken());
            for (const VkBufferMemoryBarrier2& barrier: m_resourceStates.TransitionBufferRanges({
                         .Buffer = upload.Destination->GetBuffer(),
                         .Offset = upload.DestinationOffsetBytes,
                         .Size = upload.Bytes.size(),
                         .Usage = EVulkanResourceUsage::TransferWrite,
                 })) {
                RecordVulkanResourceBarrier(arena.CommandBuffer, barrier);
            }
            const VkBufferCopy copyRegion{
                    .srcOffset = offset,
                    .dstOffset = upload.DestinationOffsetBytes,
                    .size = upload.Bytes.size(),
            };
            vkCmdCopyBuffer(arena.CommandBuffer,
                            arena.Staging.GetBuffer(),
                            upload.Destination->GetBuffer(),
                            1,
                            &copyRegion);
        }
        for (const VulkanImageUpload& upload: images) {
            const std::uint64_t offset = offsets[index++];
            std::memcpy(arena.Mapped + offset, upload.Bytes.data(), upload.Bytes.size());
            arena.Destinations.push_back(upload.Destination->GetRetentionToken());
            const NGraphics::ImageDescriptor& descriptor = upload.Destination->GetDescriptor();
            const VkImageAspectFlags aspect = GetImageAspect(descriptor.Format);
            const VkImageSubresourceRange range{
                    .aspectMask = aspect,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
            };
            if (const auto barrier = m_resourceStates.TransitionImage({
                        .Image = upload.Destination->GetImage(),
                        .Usage = EVulkanResourceUsage::TransferWrite,
                        .SubresourceRange = range,
                })) {
                RecordVulkanResourceBarrier(arena.CommandBuffer, *barrier);
            }
            const VkBufferImageCopy copyRegion{
                    .bufferOffset = offset,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = {.aspectMask = aspect, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                    .imageOffset = {.x = 0, .y = 0, .z = 0},
                    .imageExtent = {.width = descriptor.Extent.Width,
                                    .height = descriptor.Extent.Height,
                                    .depth = descriptor.Extent.Depth},
            };
            vkCmdCopyBufferToImage(arena.CommandBuffer,
                                   arena.Staging.GetBuffer(),
                                   upload.Destination->GetImage(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1,
                                   &copyRegion);
            if (const auto barrier = m_resourceStates.TransitionImage({
                        .Image = upload.Destination->GetImage(),
                        .Usage = upload.FinalUsage,
                        .SubresourceRange = range,
                })) {
                RecordVulkanResourceBarrier(arena.CommandBuffer, *barrier);
            }
        }
        // VMA rounds non-coherent flushes to nonCoherentAtomSize and skips coherent memory.
        arena.Staging.Flush(0, sizeBytes);
        EndCommandBuffer(arena.CommandBuffer);
        const VkCommandBufferSubmitInfo commandBufferInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = arena.CommandBuffer,
                .deviceMask = 0,
        };
        const VulkanQueueCompletion completion = m_submissions.SubmitGraphics({.WaitSemaphores = {},
                                                                               .CommandBuffers = {commandBufferInfo},
                                                                               .SignalSemaphores = {},
                                                                               .Fence = VK_NULL_HANDLE});
        // Nothing after successful submit may allocate or fail: all retention is already installed.
        arena.Completion = completion;
        arena.PayloadBytes = payloadBytes;
        arena.UploadCount = offsets.size();
        stateTransaction.Commit();
        m_statistics.SubmittedBytes += payloadBytes;
        m_statistics.SubmittedUploads += offsets.size();
        ++m_statistics.Submissions;
        m_statistics.PendingHighWaterBytes = std::max(m_statistics.PendingHighWaterBytes, GetStatistics().PendingBytes);
        return completion;
    } catch (...) {
        arena.Destinations.clear();
        throw;
    }
}

VulkanUploadManager::StagingArena& VulkanUploadManager::AcquireArena(std::uint64_t sizeBytes) {
    if (sizeBytes > m_config.MaxStagingBytes) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan upload batch exceeds staging budget");
    }
    for (const auto& arena: m_arenas) {
        if (arena->Completion.Value == 0 && arena->Staging.GetSizeBytes() >= sizeBytes) {
            return *arena;
        }
    }
    const std::uint64_t capacity = std::max(m_config.ArenaSizeBytes, sizeBytes);
    // Evict only idle arenas when a larger batch needs the bounded staging budget.
    for (auto arena = m_arenas.begin();
         capacity > m_config.MaxStagingBytes - m_statistics.StagingBytes && arena != m_arenas.end();) {
        if ((*arena)->Completion.Value == 0) {
            m_statistics.StagingBytes -= (*arena)->Staging.GetSizeBytes();
            vkFreeCommandBuffers(m_handle, m_commandPool, 1, &(*arena)->CommandBuffer);
            arena = m_arenas.erase(arena);
        } else {
            ++arena;
        }
    }
    if (capacity > m_config.MaxStagingBytes - m_statistics.StagingBytes) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Vulkan staging budget exhausted; retry after completion");
    }
    m_arenas.reserve(m_arenas.size() + 1);
    auto arena = std::make_unique<StagingArena>();
    arena->Staging = CreateVulkanStagingBuffer(m_memoryAllocator, capacity);
    arena->Mapped = static_cast<std::byte*>(arena->Staging.Map());
    arena->CommandBuffer = AllocateCommandBuffer();
    m_arenas.push_back(std::move(arena));
    m_statistics.StagingBytes += capacity;
    m_statistics.StagingHighWaterBytes = std::max(m_statistics.StagingHighWaterBytes, m_statistics.StagingBytes);
    ++m_statistics.ArenaAllocations;
    return *m_arenas.back();
}

void VulkanUploadManager::ReleaseCompleted() {
    for (const auto& arena: m_arenas) {
        if (arena->Completion.Value != 0 && m_submissions.IsCompleted(arena->Completion)) {
            arena->Completion = {};
            arena->PayloadBytes = 0;
            arena->UploadCount = 0;
            arena->Destinations.clear();
        }
    }
}

std::size_t VulkanUploadManager::GetPendingUploadCount() const noexcept {
    return GetStatistics().PendingUploads;
}

VulkanUploadStatistics VulkanUploadManager::GetStatistics() const noexcept {
    VulkanUploadStatistics statistics = m_statistics;
    for (const auto& arena: m_arenas) {
        if (arena->Completion.Value != 0) {
            statistics.PendingBytes += arena->PayloadBytes;
            statistics.PendingUploads += arena->UploadCount;
            ++statistics.PendingBatches;
        }
    }
    return statistics;
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
