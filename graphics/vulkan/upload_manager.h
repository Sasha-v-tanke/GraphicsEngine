#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <vulkan.h>

#include <GraphicsEngine/graphics/vulkan/device.h>
#include <GraphicsEngine/graphics/vulkan/memory.h>
#include <GraphicsEngine/graphics/vulkan/resource_state.h>
#include <GraphicsEngine/graphics/vulkan/submission_manager.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NVulkan {

struct VulkanBufferUpload {
    VulkanBufferAllocation* Destination = nullptr;
    std::span<const std::byte> Bytes;
    std::uint64_t DestinationOffsetBytes = 0;
};

struct VulkanImageUpload {
    VulkanImageAllocation* Destination = nullptr;
    std::span<const std::byte> Bytes;
    EVulkanResourceUsage FinalUsage = EVulkanResourceUsage::ShaderSampledRead;
};

struct VulkanUploadConfig {
    std::uint64_t ArenaSizeBytes = 256 * 1024;
    std::uint64_t MaxStagingBytes = 64 * 1024 * 1024;
    std::size_t MaxBatchUploads = 256;
};

struct VulkanUploadStatistics {
    std::uint64_t StagingBytes = 0;
    std::uint64_t StagingHighWaterBytes = 0;
    std::uint64_t PendingBytes = 0;
    std::uint64_t PendingHighWaterBytes = 0;
    std::size_t PendingUploads = 0;
    std::size_t PendingBatches = 0;
    std::uint64_t SubmittedBytes = 0;
    std::uint64_t SubmittedUploads = 0;
    std::uint64_t Submissions = 0;
    std::uint64_t ArenaAllocations = 0;
};

// Externally serialized with the resource-state tracker and other graphics recording.
// Device, allocator, submissions and tracker must outlive this manager.
class VulkanUploadManager final: public NCommon::NonTransferable {
public:
    VulkanUploadManager(const VulkanDevice& device,
                        VulkanMemoryAllocator& memoryAllocator,
                        VulkanSubmissionManager& submissions,
                        VulkanResourceStateTracker& resourceStates,
                        VulkanUploadConfig config = {});

    ~VulkanUploadManager() noexcept;

    [[nodiscard]] VulkanQueueCompletion UploadBuffer(VulkanBufferAllocation& destination,
                                                     std::span<const std::byte> bytes,
                                                     std::uint64_t destinationOffsetBytes = 0);

    [[nodiscard]] VulkanQueueCompletion
    UploadImage(VulkanImageAllocation& destination,
                std::span<const std::byte> bytes,
                EVulkanResourceUsage finalUsage = EVulkanResourceUsage::ShaderSampledRead);

    // One command buffer/submission, buffers first then images, preserving order within each span.
    // Copies bytes during this call and retains destinations until the returned exact completion.
    // GPU readiness belongs to the caller and may be published only after that completion.
    // Empty/oversized batches and exhausted staging budget throw without waiting or submitting.
    [[nodiscard]] VulkanQueueCompletion UploadBatch(std::span<const VulkanBufferUpload> buffers,
                                                    std::span<const VulkanImageUpload> images = {});

    void ReleaseCompleted();

    [[nodiscard]] std::size_t GetPendingUploadCount() const noexcept;
    [[nodiscard]] VulkanUploadStatistics GetStatistics() const noexcept;

private:
    struct StagingArena {
        VulkanBufferAllocation Staging;
        std::byte* Mapped = nullptr;
        VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
        VulkanQueueCompletion Completion;
        std::uint64_t PayloadBytes = 0;
        std::size_t UploadCount = 0;
        std::vector<std::shared_ptr<void>> Destinations;
    };

    [[nodiscard]] StagingArena& AcquireArena(std::uint64_t sizeBytes);
    [[nodiscard]] VkCommandPool CreateCommandPool() const;
    [[nodiscard]] VkCommandBuffer AllocateCommandBuffer() const;

private:
    const VulkanDevice& m_device;
    VulkanMemoryAllocator& m_memoryAllocator;
    VulkanSubmissionManager& m_submissions;
    VulkanResourceStateTracker& m_resourceStates;
    VulkanUploadConfig m_config;
    VulkanUploadStatistics m_statistics;
    VkDevice m_handle = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<StagingArena>> m_arenas;
};

} // namespace NVulkan
