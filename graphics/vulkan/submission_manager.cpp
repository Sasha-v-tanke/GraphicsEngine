#include "submission_manager.h"

#include <exception>
#include <limits>
#include <string_view>

#include <graphics/backend/submission_uses.h>
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
    , m_handle(device.GetHandle())
    , m_retirements(std::make_unique<NGraphics::NBackend::SubmissionRetirementQueue>()) {
    if (m_handle == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan submission manager requires a device");
    }

    m_graphicsTimeline = CreateTimelineSemaphore();
}

VulkanSubmissionManager::~VulkanSubmissionManager() {
    try {
        static_cast<void>(Shutdown());
    } catch (...) {
        // Returning would destroy owners still possibly referenced by native work.
        std::terminate();
    }

    if (m_graphicsTimeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_handle, m_graphicsTimeline, nullptr);
        m_graphicsTimeline = VK_NULL_HANDLE;
    }
}

EVulkanSubmissionShutdown VulkanSubmissionManager::Shutdown() {
    NGraphics::NBackend::SubmissionRetirementQueue::RetiredUses retired;
    {
        const VulkanLockedQueue queue = m_device.LockGraphicsQueue();
        m_shutdown = true;
        if (m_drained) {
            return m_deviceLost ? EVulkanSubmissionShutdown::DEVICE_LOST : EVulkanSubmissionShutdown::DRAINED;
        }
        const VkResult result = vkQueueWaitIdle(queue.GetHandle());
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to drain Vulkan submission uses: {}",
                                  GetVkResultName(result));
        }
        m_deviceLost = m_deviceLost || result == VK_ERROR_DEVICE_LOST;
        retired = m_retirements->TakeAfterShutdown();
        m_drained = true;
    }

    // Both successful idle and device loss end native use; never report failure as
    // ordinary timeline completion. Dependencies of the retained owners still exist.
    retired.clear();

    return m_deviceLost ? EVulkanSubmissionShutdown::DEVICE_LOST : EVulkanSubmissionShutdown::DRAINED;
}

VulkanQueueCompletion VulkanSubmissionManager::SubmitGraphics(const VulkanQueueSubmitInfo& submitInfo) {
    NGraphics::NBackend::SubmissionRetirementQueue::RetiredUses retired;
    const VulkanLockedQueue graphicsQueue = m_device.LockGraphicsQueue();
    if (m_shutdown || m_deviceLost || m_nextGraphicsValue == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Vulkan submission admission is closed");
    }
    const std::uint64_t completionValue = m_nextGraphicsValue;

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

    auto uses = m_retirements->Prepare(completionValue, submitInfo.ResourceUses);
    const VkResult result = vkQueueSubmit2(graphicsQueue.GetHandle(), 1, &vulkanSubmitInfo, submitInfo.Fence);
    if (result != VK_SUCCESS) {
        if (result == VK_ERROR_DEVICE_LOST) {
            m_deviceLost = true;
            uses.Commit(); // Submission acceptance is uncertain; retain until failure cleanup.
            retired = m_retirements->TakeAfterShutdown();
        }
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to submit Vulkan graphics queue work: {}",
                              GetVkResultName(result));
    }

    uses.Commit();
    ++m_nextGraphicsValue;

    return {
            .Value = completionValue,
    };
}

bool VulkanSubmissionManager::IsCompleted(VulkanQueueCompletion completion) const {
    NGraphics::NBackend::SubmissionRetirementQueue::RetiredUses retired;
    if (completion.Value == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan queue completion value must be valid");
    }

    const VulkanLockedQueue graphicsQueue = m_device.LockGraphicsQueue();
    if (m_deviceLost) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Vulkan device was lost");
    }
    std::uint64_t currentValue = 0;
    const VkResult result = vkGetSemaphoreCounterValue(m_handle, m_graphicsTimeline, &currentValue);
    if (result != VK_SUCCESS) {
        m_deviceLost = result == VK_ERROR_DEVICE_LOST;
        if (m_deviceLost) {
            retired = m_retirements->TakeAfterShutdown();
        }
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to query Vulkan graphics timeline: {}",
                              GetVkResultName(result));
    }

    retired = m_retirements->TakeCompleted(currentValue);

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
        if (result == VK_ERROR_DEVICE_LOST) {
            NGraphics::NBackend::SubmissionRetirementQueue::RetiredUses retired;
            const VulkanLockedQueue queue = m_device.LockGraphicsQueue();
            m_deviceLost = true;
            retired = m_retirements->TakeAfterShutdown();
        }
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to wait for Vulkan graphics timeline: {}",
                              GetVkResultName(result));
    }
    ReleaseCompleted();
}

void VulkanSubmissionManager::ReleaseCompleted() const {
    // This is only a nonblocking counter query, including before the first submission.
    static_cast<void>(IsCompleted({.Value = 1}));
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
