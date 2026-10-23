#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
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

    [[nodiscard]] VulkanClearPresentResult Render(std::uint64_t frameIndex, VkClearColorValue clearColor);

    [[nodiscard]] std::vector<VulkanFrameCompletionEvent> PollCompletedFrames();

private:
    struct FrameResources;

    [[nodiscard]] bool TryRecreateSwapchain();

    [[nodiscard]] FrameResources& GetFrameResources(VulkanFrameContext& context);

    void RecordClear(VkCommandBuffer commandBuffer, VkImage image, VkClearColorValue clearColor) const;

private:
    const VulkanDevice& m_device;
    VulkanSwapchain& m_swapchain;
    VulkanSubmissionManager m_submissions;
    VulkanFrameContextRing m_frames;
    std::vector<std::unique_ptr<FrameResources>> m_frameResources;
    std::vector<std::unique_ptr<FrameResources>> m_presentResources;
    std::deque<VulkanFrameCompletionEvent> m_pendingCompletions;
    NWindow::WindowSize m_framebufferSize;
    std::optional<VulkanQueueCompletion> m_lastSubmission;
    bool m_recreationRequested = false;
    bool m_failed = false;
};

} // namespace NVulkan
