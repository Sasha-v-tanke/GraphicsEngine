#include "clear_present_path.h"

#include <optional>
#include <string_view>

#include <GraphicsEngine/graphics/vulkan/resource_state.h>
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
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

VkSemaphore CreateSemaphore(VkDevice device) {
    const VkSemaphoreCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    const VkResult result = vkCreateSemaphore(device, &createInfo, nullptr, &semaphore);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to create Vulkan frame semaphore: {}",
                              GetVkResultName(result));
    }
    return semaphore;
}

VkCommandBuffer AllocateCommandBuffer(VkDevice device, VkCommandPool commandPool) {
    const VkCommandBufferAllocateInfo allocateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
    };
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    const VkResult result = vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to allocate Vulkan frame command buffer: {}",
                              GetVkResultName(result));
    }
    return commandBuffer;
}

VkImageSubresourceRange ColorRange() {
    return {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
    };
}

void RecordBarrier(VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2& barrier) {
    const VkDependencyInfo dependencyInfo{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
}

} // namespace

struct VulkanClearPresentPath::FrameResources {
    VkDevice Device = VK_NULL_HANDLE;
    VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
    VkSemaphore ImageAvailable = VK_NULL_HANDLE;
    VkSemaphore PresentReady = VK_NULL_HANDLE;

    ~FrameResources() {
        if (ImageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(Device, ImageAvailable, nullptr);
        }
        if (PresentReady != VK_NULL_HANDLE) {
            vkDestroySemaphore(Device, PresentReady, nullptr);
        }
    }
};

VulkanClearPresentPath::VulkanClearPresentPath(const VulkanDevice& device,
                                               VulkanSwapchain& swapchain,
                                               std::size_t maxActiveFrames)
    : m_device(device)
    , m_swapchain(swapchain)
    , m_submissions(device)
    , m_frames(device, maxActiveFrames)
    , m_frameResources(maxActiveFrames)
    , m_presentResources(swapchain.GetImages().size())
    , m_framebufferSize{.Width = static_cast<int>(swapchain.GetPlan().Extent.width),
                        .Height = static_cast<int>(swapchain.GetPlan().Extent.height)} {
}

VulkanClearPresentPath::~VulkanClearPresentPath() {
    if (m_device.GetHandle() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device.GetHandle());
    }
}

void VulkanClearPresentPath::SetFramebufferSize(NWindow::WindowSize framebufferSize) noexcept {
    if (framebufferSize.Width != m_framebufferSize.Width || framebufferSize.Height != m_framebufferSize.Height) {
        m_framebufferSize = framebufferSize;
        m_recreationRequested = true;
    }
}

bool VulkanClearPresentPath::TryRecreateSwapchain() {
    if (m_lastSubmission.has_value() && !m_submissions.IsCompleted(*m_lastSubmission)) {
        return false;
    }
    if (m_swapchain.GetHandle() != VK_NULL_HANDLE) {
        const VulkanLockedQueue queue = m_device.LockPresentQueue();
        const VkResult result = vkQueueWaitIdle(queue.GetHandle());
        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to drain Vulkan presentation queue: {}",
                                  static_cast<int>(result));
        }
    }
    m_swapchain.Recreate(m_framebufferSize);
    m_presentResources.clear();
    m_presentResources.resize(m_swapchain.GetImages().size());
    m_recreationRequested = m_swapchain.IsSuspended();
    return true;
}

VulkanClearPresentResult VulkanClearPresentPath::Render(std::uint64_t frameIndex, VkClearColorValue clearColor) {
    if (m_failed) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Vulkan clear/present path requires shutdown after failure");
    }
    if (m_framebufferSize.Width <= 0 || m_framebufferSize.Height <= 0) {
        m_swapchain.Recreate(m_framebufferSize);
        m_recreationRequested = true;
        return {.Status = EVulkanClearPresentStatus::SUSPENDED};
    }
    try {
        if (m_recreationRequested && !TryRecreateSwapchain()) {
            return {.Status = EVulkanClearPresentStatus::NOT_READY};
        }
    } catch (...) {
        m_failed = true;
        throw;
    }
    if (m_swapchain.IsSuspended()) {
        return {.Status = EVulkanClearPresentStatus::SUSPENDED};
    }

    const VulkanFrameContextAcquire acquire{
            .FrameIndex = frameIndex,
            .FrameSlotIndex = static_cast<std::size_t>(frameIndex % m_frames.GetSize()),
    };
    VulkanFrameContext* const context = m_frames.TryAcquire(acquire, [this](std::uint64_t value) {
        return m_submissions.IsCompleted({.Value = value});
    });
    if (context == nullptr) {
        return {.Status = EVulkanClearPresentStatus::NOT_READY};
    }

    bool submitted = false;
    try {
        FrameResources& resources = GetFrameResources(*context);
        const VulkanSwapchainAcquireResult acquired =
                m_swapchain.AcquireNextImage(resources.ImageAvailable, VK_NULL_HANDLE, 0);
        if (acquired.Status == EVulkanSwapchainAcquireStatus::OUT_OF_DATE) {
            m_recreationRequested = true;
            m_frames.ReleaseAcquired(acquire);
            return {.Status = EVulkanClearPresentStatus::OUT_OF_DATE};
        }
        if (acquired.Status == EVulkanSwapchainAcquireStatus::SUSPENDED) {
            m_frames.ReleaseAcquired(acquire);
            return {.Status = EVulkanClearPresentStatus::SUSPENDED};
        }
        if (acquired.Status != EVulkanSwapchainAcquireStatus::ACQUIRED &&
            acquired.Status != EVulkanSwapchainAcquireStatus::SUBOPTIMAL) {
            m_frames.ReleaseAcquired(acquire);
            return {.Status = EVulkanClearPresentStatus::NOT_READY};
        }

        std::unique_ptr<FrameResources>& presentResources = m_presentResources[acquired.ImageIndex];
        if (presentResources == nullptr) {
            auto created = std::make_unique<FrameResources>();
            created->Device = m_device.GetHandle();
            created->PresentReady = CreateSemaphore(m_device.GetHandle());
            presentResources = std::move(created);
        }
        RecordClear(resources.CommandBuffer, m_swapchain.GetImages()[acquired.ImageIndex].Handle, clearColor);
        const VkSemaphoreSubmitInfo waitSemaphore{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = resources.ImageAvailable,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        const VkCommandBufferSubmitInfo commandBuffer{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = resources.CommandBuffer,
        };
        const VkSemaphoreSubmitInfo signalSemaphore{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = presentResources->PresentReady,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        const VulkanQueueCompletion completion = m_submissions.SubmitGraphics({
                .WaitSemaphores = {waitSemaphore},
                .CommandBuffers = {commandBuffer},
                .SignalSemaphores = {signalSemaphore},
        });
        submitted = true;
        m_lastSubmission = completion;
        m_frames.MarkSubmitted(acquire, completion.Value);
        m_pendingCompletions.push_back({
                .FrameIndex = frameIndex,
                .Completion = completion,
        });

        const VulkanLockedQueue presentQueue = m_device.LockPresentQueue();
        const EVulkanSwapchainPresentStatus present =
                m_swapchain.Present(presentQueue, acquired.ImageIndex, {presentResources->PresentReady});
        m_recreationRequested = acquired.Status == EVulkanSwapchainAcquireStatus::SUBOPTIMAL ||
                                present == EVulkanSwapchainPresentStatus::SUBOPTIMAL ||
                                present == EVulkanSwapchainPresentStatus::OUT_OF_DATE;
        return {
                .Status = present == EVulkanSwapchainPresentStatus::OUT_OF_DATE ? EVulkanClearPresentStatus::OUT_OF_DATE
                                                                                : EVulkanClearPresentStatus::PRESENTED,
                .Completion = completion,
        };
    } catch (...) {
        m_failed = true;
        if (!submitted) {
            m_frames.ReleaseAcquired(acquire);
        }
        throw;
    }
}

std::vector<VulkanFrameCompletionEvent> VulkanClearPresentPath::PollCompletedFrames() {
    std::vector<VulkanFrameCompletionEvent> completed;
    while (!m_pendingCompletions.empty() && m_submissions.IsCompleted(m_pendingCompletions.front().Completion)) {
        completed.push_back(m_pendingCompletions.front());
        m_pendingCompletions.pop_front();
    }
    return completed;
}

VulkanClearPresentPath::FrameResources& VulkanClearPresentPath::GetFrameResources(VulkanFrameContext& context) {
    std::unique_ptr<FrameResources>& resources = m_frameResources[context.GetFrameSlotIndex()];
    if (resources == nullptr) {
        resources = std::make_unique<FrameResources>();
        resources->Device = m_device.GetHandle();
        resources->CommandBuffer = AllocateCommandBuffer(m_device.GetHandle(), context.GetCommandPool());
        resources->ImageAvailable = CreateSemaphore(m_device.GetHandle());
    }
    return *resources;
}

void VulkanClearPresentPath::RecordClear(VkCommandBuffer commandBuffer,
                                         VkImage image,
                                         VkClearColorValue clearColor) const {
    const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to begin Vulkan frame command buffer: {}",
                              GetVkResultName(result));
    }

    VulkanResourceStateTracker tracker;
    tracker.ImportImage(image, ColorRange(), MakeVulkanImageState(EVulkanResourceUsage::Undefined));
    const std::optional<VkImageMemoryBarrier2> colorBarrier = tracker.TransitionImage({
            .Image = image,
            .Usage = EVulkanResourceUsage::TransferWrite,
            .SubresourceRange = ColorRange(),
    });
    RecordBarrier(commandBuffer, *colorBarrier);
    const VkImageSubresourceRange colorRange = ColorRange();
    vkCmdClearColorImage(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &colorRange);

    const std::optional<VkImageMemoryBarrier2> presentBarrier = tracker.TransitionImage({
            .Image = image,
            .Usage = EVulkanResourceUsage::Present,
            .SubresourceRange = ColorRange(),
    });
    RecordBarrier(commandBuffer, *presentBarrier);
    result = vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to end Vulkan frame command buffer: {}",
                              GetVkResultName(result));
    }
}

} // namespace NVulkan
