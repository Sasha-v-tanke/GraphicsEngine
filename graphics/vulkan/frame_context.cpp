#include "frame_context.h"

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

[[nodiscard]] VkCommandPool CreateCommandPool(VkDevice device, std::uint32_t queueFamilyIndex) {
    const VkCommandPoolCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queueFamilyIndex,
    };

    VkCommandPool commandPool = VK_NULL_HANDLE;
    const VkResult result = vkCreateCommandPool(device, &createInfo, nullptr, &commandPool);

    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to create Vulkan frame command pool: {}",
                              GetVkResultName(result));
    }

    return commandPool;
}

void ResetCommandPool(VkDevice device, VkCommandPool commandPool) {
    const VkResult result = vkResetCommandPool(device, commandPool, 0);

    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to reset Vulkan frame command pool: {}",
                              GetVkResultName(result));
    }
}

} // namespace

VulkanFrameContext::VulkanFrameContext(VkDevice device, std::size_t frameSlotIndex, std::uint32_t queueFamilyIndex)
    : m_device(device)
    , m_frameSlotIndex(frameSlotIndex)
    , m_commandPool(CreateCommandPool(device, queueFamilyIndex)) {
}

VulkanFrameContext::~VulkanFrameContext() {
    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
}

std::uint64_t VulkanFrameContext::GetFrameIndex() const noexcept {
    return m_frameIndex;
}

std::size_t VulkanFrameContext::GetFrameSlotIndex() const noexcept {
    return m_frameSlotIndex;
}

VkCommandPool VulkanFrameContext::GetCommandPool() const noexcept {
    return m_commandPool;
}

std::optional<std::uint64_t> VulkanFrameContext::GetCompletionValue() const noexcept {
    return m_completionValue;
}

void VulkanFrameContext::Acquire(std::uint64_t frameIndex) {
    if (IsInFlight()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Vulkan frame context {} cannot be acquired before completion {}",
                              m_frameSlotIndex,
                              *m_completionValue);
    }

    ResetCommandPool(m_device, m_commandPool);
    m_frameIndex = frameIndex;
    m_acquired = true;
}

void VulkanFrameContext::MarkSubmitted(std::uint64_t completionValue) {
    if (completionValue == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan frame completion value must be valid");
    }

    m_completionValue = completionValue;
}

void VulkanFrameContext::MarkCompleted() noexcept {
    m_completionValue.reset();
}

bool VulkanFrameContext::IsInFlight() const noexcept {
    return m_completionValue.has_value();
}

bool VulkanFrameContext::MatchesFrame(std::uint64_t frameIndex) const noexcept {
    return m_acquired && m_frameIndex == frameIndex;
}

VulkanFrameContextRing::VulkanFrameContextRing(const VulkanDevice& device, std::size_t maxActiveFrames)
    : m_device(device.GetHandle()) {
    if (m_device == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan frame context ring requires a device");
    }

    if (maxActiveFrames == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan frame context ring size must be greater than zero");
    }

    VulkanLockedQueue graphicsQueue = device.LockGraphicsQueue();
    m_contexts.reserve(maxActiveFrames);

    for (std::size_t frameSlotIndex = 0; frameSlotIndex < maxActiveFrames; ++frameSlotIndex) {
        m_contexts.emplace_back(std::unique_ptr<VulkanFrameContext>{
                new VulkanFrameContext{m_device, frameSlotIndex, graphicsQueue.GetFamilyIndex()}});
    }
}

VulkanFrameContextRing::~VulkanFrameContextRing() {
    for (const std::unique_ptr<VulkanFrameContext>& context: m_contexts) {
        if (context != nullptr && context->IsInFlight()) {
            vkDeviceWaitIdle(m_device);
            break;
        }
    }
}

std::size_t VulkanFrameContextRing::GetSize() const noexcept {
    return m_contexts.size();
}

VulkanFrameContext* VulkanFrameContextRing::TryAcquire(VulkanFrameContextAcquire acquire,
                                                       const CompletionQuery& isCompleted) {
    VulkanFrameContext& context = GetBySlot(acquire.FrameSlotIndex);

    if (context.IsInFlight()) {
        if (!isCompleted || !isCompleted(*context.GetCompletionValue())) {
            return nullptr;
        }

        context.MarkCompleted();
    }

    context.Acquire(acquire.FrameIndex);
    return &context;
}

VulkanFrameContext& VulkanFrameContextRing::Get(VulkanFrameContextAcquire acquire) {
    VulkanFrameContext& context = GetBySlot(acquire.FrameSlotIndex);

    if (!context.MatchesFrame(acquire.FrameIndex)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan frame context {} does not match frame {}",
                              acquire.FrameSlotIndex,
                              acquire.FrameIndex);
    }

    return context;
}

void VulkanFrameContextRing::MarkSubmitted(VulkanFrameContextAcquire acquire, std::uint64_t completionValue) {
    Get(acquire).MarkSubmitted(completionValue);
}

void VulkanFrameContextRing::MarkCompleted(VulkanFrameContextAcquire acquire) {
    Get(acquire).MarkCompleted();
}

VulkanFrameContext& VulkanFrameContextRing::GetBySlot(std::size_t frameSlotIndex) {
    if (frameSlotIndex >= m_contexts.size()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan frame slot {} is out of range [0, {})",
                              frameSlotIndex,
                              m_contexts.size());
    }

    return *m_contexts[frameSlotIndex];
}

} // namespace NVulkan
