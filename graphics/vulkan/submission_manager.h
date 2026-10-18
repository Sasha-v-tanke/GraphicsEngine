#pragma once

#include <cstdint>
#include <vector>
#include <vulkan.h>

#include <GraphicsEngine/graphics/vulkan/device.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NVulkan {

struct VulkanQueueCompletion {
    std::uint64_t Value = 0;
};

struct VulkanQueueSubmitInfo {
    std::vector<VkSemaphoreSubmitInfo> WaitSemaphores;
    std::vector<VkCommandBufferSubmitInfo> CommandBuffers;
    std::vector<VkSemaphoreSubmitInfo> SignalSemaphores;
    VkFence Fence = VK_NULL_HANDLE;
};

class VulkanSubmissionManager final: public NCommon::NonTransferable {
public:
    explicit VulkanSubmissionManager(const VulkanDevice& device);

    ~VulkanSubmissionManager();

    [[nodiscard]] VulkanQueueCompletion SubmitGraphics(const VulkanQueueSubmitInfo& submitInfo);

    [[nodiscard]] bool IsCompleted(VulkanQueueCompletion completion) const;

    void Wait(VulkanQueueCompletion completion, std::uint64_t timeoutNanoseconds) const;

private:
    [[nodiscard]] VkSemaphore CreateTimelineSemaphore() const;

private:
    const VulkanDevice& m_device;
    VkDevice m_handle = VK_NULL_HANDLE;
    VkSemaphore m_graphicsTimeline = VK_NULL_HANDLE;
    std::uint64_t m_nextGraphicsValue = 1;
};

} // namespace NVulkan
