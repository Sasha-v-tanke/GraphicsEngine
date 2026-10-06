#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <vulkan.h>

#include <GraphicsEngine/graphics/vulkan/device.h>
#include <GraphicsEngine/graphics/vulkan/physical_device.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NVulkan {

struct VulkanSwapchainSupport {
    VkSurfaceCapabilitiesKHR Capabilities{};
    std::vector<VkSurfaceFormatKHR> Formats;
    std::vector<VkPresentModeKHR> PresentModes;
};

struct VulkanSwapchainConfig {
    VkFormat ImageFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR ColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR PresentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D Extent{};
    std::uint32_t ImageCount = 0;
    VkSharingMode SharingMode = VK_SHARING_MODE_EXCLUSIVE;
    std::vector<std::uint32_t> QueueFamilyIndices;
    VkSurfaceTransformFlagBitsKHR PreTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkCompositeAlphaFlagBitsKHR CompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    bool Suspended = false;
};

[[nodiscard]] VulkanSwapchainConfig MakeVulkanSwapchainConfig(const VulkanSwapchainSupport& support,
                                                              VkExtent2D framebufferExtent,
                                                              std::uint32_t graphicsQueueFamilyIndex,
                                                              std::uint32_t presentQueueFamilyIndex);

[[nodiscard]] VulkanSwapchainSupport ReadVulkanSwapchainSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface);

class VulkanSwapchain final: public NCommon::NonTransferable {
public:
    VulkanSwapchain(const VulkanPhysicalDeviceSelection& physicalDevice,
                    const VulkanDevice& device,
                    VkSurfaceKHR surface,
                    VkExtent2D framebufferExtent);

    ~VulkanSwapchain();

    [[nodiscard]] bool IsSuspended() const noexcept;

    [[nodiscard]] VkSwapchainKHR GetHandle() const noexcept;

    [[nodiscard]] VkFormat GetImageFormat() const noexcept;

    [[nodiscard]] VkExtent2D GetExtent() const noexcept;

    [[nodiscard]] std::span<const VkImage> GetImages() const noexcept;

    [[nodiscard]] std::span<const VkImageView> GetImageViews() const noexcept;

    [[nodiscard]] std::optional<std::uint32_t>
    AcquireNextImage(std::uint64_t timeoutNanoseconds, VkSemaphore semaphore, VkFence fence) const;

    void Present(VulkanLockedQueue presentQueue,
                 std::uint32_t imageIndex,
                 std::span<const VkSemaphore> waitSemaphores) const;

private:
    class Impl;

    std::unique_ptr<Impl> m_impl;
};

} // namespace NVulkan
