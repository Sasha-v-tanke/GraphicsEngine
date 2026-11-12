#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan.h>

#include <GraphicsEngine/graphics/vulkan/device.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NGraphics::NBackend {
class SubmissionRetirementQueue;
}

namespace NVulkan {

struct VulkanQueueCompletion {
    std::uint64_t Value = 0;
};

enum class EVulkanSubmissionShutdown {
    DRAINED,
    DEVICE_LOST,
};

struct VulkanQueueSubmitInfo {
    std::vector<VkSemaphoreSubmitInfo> WaitSemaphores;
    std::vector<VkCommandBufferSubmitInfo> CommandBuffers;
    std::vector<VkSemaphoreSubmitInfo> SignalSemaphores;
    VkFence Fence = VK_NULL_HANDLE;
    std::vector<std::shared_ptr<const void>> ResourceUses = {};
};

class VulkanSubmissionManager final: public NCommon::NonTransferable {
public:
    explicit VulkanSubmissionManager(const VulkanDevice& device);

    ~VulkanSubmissionManager();

    [[nodiscard]] VulkanQueueCompletion SubmitGraphics(const VulkanQueueSubmitInfo& submitInfo);

    [[nodiscard]] bool IsCompleted(VulkanQueueCompletion completion) const;

    void Wait(VulkanQueueCompletion completion, std::uint64_t timeoutNanoseconds) const;
    // Nonblocking collection of representation owners after observed timeline completion.
    void ReleaseCompleted() const;
    // Host-only shutdown, after all publishers/recorders stop. Closes admission.
    // Unexpected driver errors preserve owners and throw; callers may retry. The
    // destructor fails closed if an explicit/retried drain cannot establish safety.
    [[nodiscard]] EVulkanSubmissionShutdown Shutdown();

private:
    [[nodiscard]] VkSemaphore CreateTimelineSemaphore() const;

private:
    const VulkanDevice& m_device;
    VkDevice m_handle = VK_NULL_HANDLE;
    VkSemaphore m_graphicsTimeline = VK_NULL_HANDLE;
    std::uint64_t m_nextGraphicsValue = 1;
    std::unique_ptr<NGraphics::NBackend::SubmissionRetirementQueue> m_retirements;
    mutable bool m_deviceLost = false;
    bool m_shutdown = false;
    bool m_drained = false;
};

} // namespace NVulkan
