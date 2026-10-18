#include "submission_manager.h"

#include <string_view>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

namespace {

std::string_view GetVkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
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

} // namespace

VulkanSubmissionManager::VulkanSubmissionManager(const VulkanDevice& device)
    : m_device(device)
    , m_handle(device.GetHandle()) {
    if (m_handle == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan submission manager requires a device");
    }

    m_graphicsTimeline = CreateTimelineSemaphore();
}

VulkanSubmissionManager::~VulkanSubmissionManager() {
    if (m_handle != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_handle);
    }

    if (m_graphicsTimeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_handle, m_graphicsTimeline, nullptr);
        m_graphicsTimeline = VK_NULL_HANDLE;
    }
}

VulkanQueueCompletion VulkanSubmissionManager::SubmitGraphics(const VulkanQueueSubmitInfo& submitInfo) {
    const VulkanLockedQueue graphicsQueue = m_device.LockGraphicsQueue();
    const std::uint64_t completionValue = m_nextGraphicsValue++;

    std::vector<VkSemaphoreSubmitInfo> signalSemaphores = submitInfo.SignalSemaphores;
    signalSemaphores.push_back({
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = m_graphicsTimeline,
            .value = completionValue,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
    });

    const VkSubmitInfo2 vulkanSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext = nullptr,
            .flags = 0,
            .waitSemaphoreInfoCount = static_cast<std::uint32_t>(submitInfo.WaitSemaphores.size()),
            .pWaitSemaphoreInfos = submitInfo.WaitSemaphores.data(),
            .commandBufferInfoCount = static_cast<std::uint32_t>(submitInfo.CommandBuffers.size()),
            .pCommandBufferInfos = submitInfo.CommandBuffers.data(),
            .signalSemaphoreInfoCount = static_cast<std::uint32_t>(signalSemaphores.size()),
            .pSignalSemaphoreInfos = signalSemaphores.data(),
    };

    const VkResult result = vkQueueSubmit2(graphicsQueue.GetHandle(), 1, &vulkanSubmitInfo, submitInfo.Fence);
    if (result != VK_SUCCESS) {
        --m_nextGraphicsValue;
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to submit Vulkan graphics queue work: {}",
                              GetVkResultName(result));
    }

    return {
            .Value = completionValue,
    };
}

bool VulkanSubmissionManager::IsCompleted(VulkanQueueCompletion completion) const {
    if (completion.Value == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan queue completion value must be valid");
    }

    std::uint64_t currentValue = 0;
    const VkResult result = vkGetSemaphoreCounterValue(m_handle, m_graphicsTimeline, &currentValue);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to query Vulkan graphics timeline: {}",
                              GetVkResultName(result));
    }

    return currentValue >= completion.Value;
}

void VulkanSubmissionManager::Wait(VulkanQueueCompletion completion, std::uint64_t timeoutNanoseconds) const {
    if (completion.Value == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan queue completion value must be valid");
    }

    const VkSemaphoreWaitInfo waitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .pNext = nullptr,
            .flags = 0,
            .semaphoreCount = 1,
            .pSemaphores = &m_graphicsTimeline,
            .pValues = &completion.Value,
    };

    const VkResult result = vkWaitSemaphores(m_handle, &waitInfo, timeoutNanoseconds);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to wait for Vulkan graphics timeline: {}",
                              GetVkResultName(result));
    }
}

VkSemaphore VulkanSubmissionManager::CreateTimelineSemaphore() const {
    VkSemaphoreTypeCreateInfo timelineInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .pNext = nullptr,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
    };
    const VkSemaphoreCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timelineInfo,
            .flags = 0,
    };

    VkSemaphore semaphore = VK_NULL_HANDLE;
    const VkResult result = vkCreateSemaphore(m_handle, &createInfo, nullptr, &semaphore);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to create Vulkan graphics timeline semaphore: {}",
                              GetVkResultName(result));
    }

    return semaphore;
}

} // namespace NVulkan
