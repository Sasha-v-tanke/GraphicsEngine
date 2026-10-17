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

class VulkanUploadManager final: public NCommon::NonTransferable {
public:
    VulkanUploadManager(const VulkanDevice& device,
                        VulkanMemoryAllocator& memoryAllocator,
                        VulkanSubmissionManager& submissions,
                        VulkanResourceStateTracker& resourceStates);

    ~VulkanUploadManager() noexcept;

    [[nodiscard]] VulkanQueueCompletion UploadBuffer(VulkanBufferAllocation& destination,
                                                     std::span<const std::byte> bytes,
                                                     std::uint64_t destinationOffsetBytes = 0);

    [[nodiscard]] VulkanQueueCompletion
    UploadImage(VulkanImageAllocation& destination,
                std::span<const std::byte> bytes,
                EVulkanResourceUsage finalUsage = EVulkanResourceUsage::ShaderSampledRead);

    void ReleaseCompleted();

    [[nodiscard]] std::size_t GetPendingUploadCount() const noexcept;

private:
    struct PendingUpload {
        VulkanQueueCompletion Completion;
        VulkanBufferAllocation Staging;
        VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
        std::shared_ptr<void> Destination;
    };

    [[nodiscard]] VkCommandPool CreateCommandPool() const;
    [[nodiscard]] VkCommandBuffer AllocateCommandBuffer() const;

private:
    const VulkanDevice& m_device;
    VulkanMemoryAllocator& m_memoryAllocator;
    VulkanSubmissionManager& m_submissions;
    VulkanResourceStateTracker& m_resourceStates;
    VkDevice m_handle = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<PendingUpload> m_pendingUploads;
};

} // namespace NVulkan
