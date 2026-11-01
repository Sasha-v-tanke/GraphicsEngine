#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <GraphicsEngine/graphics/vulkan/frame_context.h>
#include <GraphicsEngine/graphics/vulkan/submission_manager.h>
#include <GraphicsEngine/graphics/vulkan/swapchain.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NVulkan {

enum class EVulkanClearPresentStatus {
    PRESENTED,
    NOT_READY,
    OUT_OF_DATE,
    SUSPENDED,
};

struct VulkanClearPresentResult {
    EVulkanClearPresentStatus Status = EVulkanClearPresentStatus::NOT_READY;
    std::optional<VulkanQueueCompletion> Completion;
};

struct VulkanFrameCompletionEvent {
    std::uint64_t FrameIndex = 0;
    VulkanQueueCompletion Completion;
};

class VulkanClearPresentPath final: public NCommon::NonTransferable {
public:
    VulkanClearPresentPath(const VulkanDevice& device, VulkanSwapchain& swapchain, std::size_t maxActiveFrames);

    ~VulkanClearPresentPath();

    void SetFramebufferSize(NWindow::WindowSize framebufferSize) noexcept;

    // Retries before submission consume no frame. OUT_OF_DATE may carry an accepted GPU completion.
    // onSubmitted runs immediately after native submit, before fallible bookkeeping/present; it must not throw.
    // Optional record replaces clear recording after acquire; it must begin/end the primary command
    // buffer, transition the acquired image to Present, and keep every referenced object alive through completion.
    // Throwing fails the path closed. Backend integration only; never a user rendering callback.
    // The caller must retain that completion if Render subsequently throws, and stop using the failed path.
    [[nodiscard]] VulkanClearPresentResult
    Render(std::uint64_t frameIndex,
           VkClearColorValue clearColor,
           const std::function<void(VulkanQueueCompletion)>& onSubmitted = {},
           const std::function<void(VkCommandBuffer, VkImage, VkImageView, const VulkanSwapchainPlan&)>& record = {});

    [[nodiscard]] bool IsCompleted(VulkanQueueCompletion completion) const;

    [[nodiscard]] std::vector<VulkanFrameCompletionEvent> PollCompletedFrames();

private:
    struct FrameResources;

    [[nodiscard]] bool TryRecreateSwapchain();
    void ReleaseRetiredIfCompleted();

    [[nodiscard]] FrameResources& GetFrameResources(VulkanFrameContext& context);

    void
    RecordClear(VkCommandBuffer commandBuffer, VkImage image, VkImageView view, VkClearColorValue clearColor) const;

private:
    const VulkanDevice& m_device;
    VulkanSwapchain& m_swapchain;
    VulkanSubmissionManager m_submissions;
    VulkanFrameContextRing m_frames;
    std::vector<std::unique_ptr<FrameResources>> m_frameResources;
    std::vector<std::unique_ptr<FrameResources>> m_presentResources;
    std::vector<std::vector<std::unique_ptr<FrameResources>>> m_retiredPresentResources;
    std::optional<std::uint32_t> m_firstPresentedImage;
    std::deque<VulkanFrameCompletionEvent> m_pendingCompletions;
    NWindow::WindowSize m_framebufferSize;
    std::optional<VulkanQueueCompletion> m_lastSubmission;
    std::optional<VulkanQueueCompletion> m_retirementCompletion;
    bool m_recreationRequested = false;
    bool m_failed = false;
};

} // namespace NVulkan
