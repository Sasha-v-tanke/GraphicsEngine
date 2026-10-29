#pragma once

#include <cstdint>
#include <optional>
#include <vulkan.h>

namespace NVulkan::NTest {

// Inject only driver statuses; successful acquire/present still execute on the real device.
class SwapchainResultOverride {
public:
    SwapchainResultOverride() {
        OriginalAcquire = vkAcquireNextImageKHR;
        OriginalPresent = vkQueuePresentKHR;
        vkAcquireNextImageKHR = Acquire;
        vkQueuePresentKHR = Present;
    }

    ~SwapchainResultOverride() {
        vkAcquireNextImageKHR = OriginalAcquire;
        vkQueuePresentKHR = OriginalPresent;
        NextAcquire.reset();
        NextPresent.reset();
    }

    inline static std::optional<VkResult> NextAcquire;
    inline static std::optional<VkResult> NextPresent;

private:
    static VKAPI_ATTR VkResult VKAPI_CALL Acquire(VkDevice device,
                                                  VkSwapchainKHR swapchain,
                                                  std::uint64_t timeout,
                                                  VkSemaphore semaphore,
                                                  VkFence fence,
                                                  std::uint32_t* imageIndex) {
        if (NextAcquire == VK_ERROR_OUT_OF_DATE_KHR || NextAcquire == VK_NOT_READY) {
            const VkResult result = *NextAcquire;
            NextAcquire.reset();
            return result;
        }
        const VkResult result = OriginalAcquire(device, swapchain, timeout, semaphore, fence, imageIndex);
        if (NextAcquire.has_value() && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) {
            const auto overridden = *NextAcquire;
            NextAcquire.reset();
            return overridden;
        }
        return result;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue queue, const VkPresentInfoKHR* info) {
        const VkResult result = OriginalPresent(queue, info);
        if (NextPresent.has_value() && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) {
            const auto overridden = *NextPresent;
            NextPresent.reset();
            return overridden;
        }
        return result;
    }

    inline static PFN_vkAcquireNextImageKHR OriginalAcquire = nullptr;
    inline static PFN_vkQueuePresentKHR OriginalPresent = nullptr;
};

} // namespace NVulkan::NTest
