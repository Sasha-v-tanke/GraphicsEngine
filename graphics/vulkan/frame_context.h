#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <vulkan.h>

#include <GraphicsEngine/graphics/vulkan/device.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NVulkan {

struct VulkanFrameContextAcquire {
    std::uint64_t FrameIndex = 0;
    std::size_t FrameSlotIndex = 0;
};

class VulkanFrameContext final: public NCommon::NonTransferable {
public:
    ~VulkanFrameContext();

    [[nodiscard]] std::uint64_t GetFrameIndex() const noexcept;

    [[nodiscard]] std::size_t GetFrameSlotIndex() const noexcept;

    [[nodiscard]] VkCommandPool GetCommandPool() const noexcept;

    [[nodiscard]] std::optional<std::uint64_t> GetCompletionValue() const noexcept;

private:
    VulkanFrameContext(VkDevice device, std::size_t frameSlotIndex, std::uint32_t queueFamilyIndex);

    void Acquire(std::uint64_t frameIndex);

    void MarkSubmitted(std::uint64_t completionValue);

    void MarkCompleted() noexcept;

    [[nodiscard]] bool IsInFlight() const noexcept;

    [[nodiscard]] bool MatchesFrame(std::uint64_t frameIndex) const noexcept;

private:
    VkDevice m_device = VK_NULL_HANDLE;
    std::size_t m_frameSlotIndex = 0;
    std::uint64_t m_frameIndex = 0;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::optional<std::uint64_t> m_completionValue;
    bool m_acquired = false;

    friend class VulkanFrameContextRing;
};

class VulkanFrameContextRing final: public NCommon::NonTransferable {
public:
    using CompletionQuery = std::function<bool(std::uint64_t)>;

    VulkanFrameContextRing(const VulkanDevice& device, std::size_t maxActiveFrames);

    ~VulkanFrameContextRing();

    [[nodiscard]] std::size_t GetSize() const noexcept;

    [[nodiscard]] VulkanFrameContext* TryAcquire(VulkanFrameContextAcquire acquire, const CompletionQuery& isCompleted);

    [[nodiscard]] VulkanFrameContext& Get(VulkanFrameContextAcquire acquire);

    void MarkSubmitted(VulkanFrameContextAcquire acquire, std::uint64_t completionValue);

    void MarkCompleted(VulkanFrameContextAcquire acquire);

private:
    [[nodiscard]] VulkanFrameContext& GetBySlot(std::size_t frameSlotIndex);

private:
    VkDevice m_device = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<VulkanFrameContext>> m_contexts;
};

} // namespace NVulkan
