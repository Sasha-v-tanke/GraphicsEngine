#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan.h>

#include <GraphicsEngine/graphics/vulkan/device.h>
#include <GraphicsEngine/graphics/vulkan/physical_device.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/window/window_size.h>

namespace NVulkan {

struct VulkanSwapchainSupport {
    VkSurfaceCapabilitiesKHR Capabilities{};
    std::vector<VkSurfaceFormatKHR> Formats;
    std::vector<VkPresentModeKHR> PresentModes;
};

struct VulkanSwapchainPlan {
    bool Suspended = false;
    VkSurfaceFormatKHR SurfaceFormat{};
    VkPresentModeKHR PresentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D Extent{};
    std::uint32_t ImageCount = 0;
    VkSharingMode SharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkSurfaceTransformFlagBitsKHR PreTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkCompositeAlphaFlagBitsKHR CompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    std::vector<std::uint32_t> QueueFamilyIndices;
};

struct VulkanSwapchainImage {
    std::uint32_t Index = 0;
    VkImage Handle = VK_NULL_HANDLE;
    VkImageView View = VK_NULL_HANDLE;
};

enum class EVulkanSwapchainAcquireStatus {
    ACQUIRED,
    OUT_OF_DATE,
    SUSPENDED,
};

struct VulkanSwapchainAcquireResult {
    EVulkanSwapchainAcquireStatus Status = EVulkanSwapchainAcquireStatus::SUSPENDED;
    std::uint32_t ImageIndex = 0;
};

enum class EVulkanSwapchainPresentStatus {
    PRESENTED,
    SUBOPTIMAL,
    OUT_OF_DATE,
    SUSPENDED,
};

[[nodiscard]] VulkanSwapchainSupport ReadVulkanSwapchainSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface);

[[nodiscard]] VulkanSwapchainPlan MakeVulkanSwapchainPlan(const VulkanSwapchainSupport& support,
                                                          NWindow::WindowSize framebufferSize,
                                                          std::uint32_t graphicsQueueFamilyIndex,
                                                          std::uint32_t presentQueueFamilyIndex);

class VulkanSwapchain final: public NCommon::NonTransferable {
public:
    VulkanSwapchain(const VulkanDevice& device,
                    const VulkanPhysicalDeviceSelection& physicalDevice,
                    VkSurfaceKHR surface,
                    NWindow::WindowSize framebufferSize);

    ~VulkanSwapchain();

    [[nodiscard]] bool IsSuspended() const noexcept;

    [[nodiscard]] const VulkanSwapchainPlan& GetPlan() const noexcept;

    [[nodiscard]] VkSwapchainKHR GetHandle() const noexcept;

    [[nodiscard]] const std::vector<VulkanSwapchainImage>& GetImages() const noexcept;

    [[nodiscard]] VulkanSwapchainAcquireResult AcquireNextImage(VkSemaphore signalSemaphore,
                                                                VkFence signalFence = VK_NULL_HANDLE,
                                                                std::uint64_t timeoutNanoseconds = UINT64_MAX) const;

    [[nodiscard]] EVulkanSwapchainPresentStatus Present(const VulkanLockedQueue& presentQueue,
                                                        std::uint32_t imageIndex,
                                                        const std::vector<VkSemaphore>& waitSemaphores = {}) const;

private:
    class Impl;

    std::unique_ptr<Impl> m_impl;
};

} // namespace NVulkan
