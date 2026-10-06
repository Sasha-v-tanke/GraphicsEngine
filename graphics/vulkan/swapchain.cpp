#include "swapchain.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

namespace {

constexpr VkFormat PreferredSwapchainFormat = VK_FORMAT_B8G8R8A8_SRGB;
constexpr VkColorSpaceKHR PreferredSwapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
constexpr VkPresentModeKHR PreferredPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
constexpr VkPresentModeKHR RequiredPresentMode = VK_PRESENT_MODE_FIFO_KHR;
constexpr std::uint32_t DynamicExtent = std::numeric_limits<std::uint32_t>::max();

std::string_view GetVkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

[[nodiscard]] bool HasDynamicExtent(const VkSurfaceCapabilitiesKHR& capabilities) noexcept {
    return capabilities.currentExtent.width == DynamicExtent;
}

[[nodiscard]] bool IsZeroExtent(VkExtent2D extent) noexcept {
    return extent.width == 0 || extent.height == 0;
}

[[nodiscard]] bool IsZeroFramebuffer(VkExtent2D framebufferExtent) noexcept {
    return framebufferExtent.width == 0 || framebufferExtent.height == 0;
}

[[nodiscard]] VkExtent2D ClampExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                                     VkExtent2D framebufferExtent) noexcept {
    if (IsZeroFramebuffer(framebufferExtent)) {
        return framebufferExtent;
    }

    if (!HasDynamicExtent(capabilities)) {
        return capabilities.currentExtent;
    }

    return {
            .width = std::clamp(framebufferExtent.width,
                                capabilities.minImageExtent.width,
                                capabilities.maxImageExtent.width),
            .height = std::clamp(framebufferExtent.height,
                                 capabilities.minImageExtent.height,
                                 capabilities.maxImageExtent.height),
    };
}

[[nodiscard]] VkSurfaceFormatKHR ChooseSurfaceFormat(std::span<const VkSurfaceFormatKHR> formats) {
    if (formats.empty()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan swapchain requires surface formats");
    }

    const auto preferred = std::ranges::find_if(formats, [](VkSurfaceFormatKHR format) {
        return format.format == PreferredSwapchainFormat && format.colorSpace == PreferredSwapchainColorSpace;
    });
    if (preferred != formats.end()) {
        return *preferred;
    }

    return formats.front();
}

[[nodiscard]] VkPresentModeKHR ChoosePresentMode(std::span<const VkPresentModeKHR> presentModes) {
    if (presentModes.empty()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan swapchain requires present modes");
    }

    if (std::ranges::find(presentModes, PreferredPresentMode) != presentModes.end()) {
        return PreferredPresentMode;
    }

    if (std::ranges::find(presentModes, RequiredPresentMode) != presentModes.end()) {
        return RequiredPresentMode;
    }

    return presentModes.front();
}

[[nodiscard]] std::uint32_t ChooseImageCount(const VkSurfaceCapabilitiesKHR& capabilities) noexcept {
    std::uint32_t imageCount = capabilities.minImageCount + 1U;

    if (capabilities.maxImageCount != 0U) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    return imageCount;
}

[[nodiscard]] std::vector<std::uint32_t> MakeQueueFamilyIndices(std::uint32_t graphicsQueueFamilyIndex,
                                                                std::uint32_t presentQueueFamilyIndex) {
    if (graphicsQueueFamilyIndex == presentQueueFamilyIndex) {
        return {};
    }

    return {graphicsQueueFamilyIndex, presentQueueFamilyIndex};
}

[[nodiscard]] VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) noexcept {
    if ((supported & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0) {
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }

    if ((supported & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0) {
        return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    }

    if ((supported & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) != 0) {
        return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    }

    return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
}

[[nodiscard]] VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format) {
    const VkImageViewCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components =
                    {
                            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
                    },
            .subresourceRange =
                    {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .baseMipLevel = 0,
                            .levelCount = 1,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                    },
    };

    VkImageView view = VK_NULL_HANDLE;
    const VkResult result = vkCreateImageView(device, &createInfo, nullptr, &view);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to create Vulkan swapchain image view: {}",
                              GetVkResultName(result));
    }

    return view;
}

[[nodiscard]] std::vector<VkImage> ReadSwapchainImages(VkDevice device, VkSwapchainKHR swapchain) {
    std::uint32_t count = 0;
    VkResult result = vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to read Vulkan swapchain image count: {}",
                              GetVkResultName(result));
    }

    std::vector<VkImage> images(count);
    result = vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to read Vulkan swapchain images: {}",
                              GetVkResultName(result));
    }

    images.resize(count);
    return images;
}

} // namespace

VulkanSwapchainConfig MakeVulkanSwapchainConfig(const VulkanSwapchainSupport& support,
                                                VkExtent2D framebufferExtent,
                                                std::uint32_t graphicsQueueFamilyIndex,
                                                std::uint32_t presentQueueFamilyIndex) {
    const VkSurfaceFormatKHR format = ChooseSurfaceFormat(support.Formats);
    const VkExtent2D extent = ClampExtent(support.Capabilities, framebufferExtent);
    const std::vector<std::uint32_t> queueFamilyIndices =
            MakeQueueFamilyIndices(graphicsQueueFamilyIndex, presentQueueFamilyIndex);

    return {
            .ImageFormat = format.format,
            .ColorSpace = format.colorSpace,
            .PresentMode = ChoosePresentMode(support.PresentModes),
            .Extent = extent,
            .ImageCount = ChooseImageCount(support.Capabilities),
            .SharingMode = queueFamilyIndices.empty() ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
            .QueueFamilyIndices = queueFamilyIndices,
            .PreTransform = support.Capabilities.currentTransform,
            .CompositeAlpha = ChooseCompositeAlpha(support.Capabilities.supportedCompositeAlpha),
            .Suspended = IsZeroFramebuffer(framebufferExtent) || IsZeroExtent(extent),
    };
}

VulkanSwapchainSupport ReadVulkanSwapchainSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface) {
    if (physicalDevice == VK_NULL_HANDLE || surface == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan swapchain support requires physical device and surface");
    }

    VulkanSwapchainSupport support;
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &support.Capabilities);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to read Vulkan surface capabilities: {}",
                              GetVkResultName(result));
    }

    std::uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to read Vulkan surface format count: {}",
                              GetVkResultName(result));
    }

    support.Formats.resize(formatCount);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, support.Formats.data());
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to read Vulkan surface formats: {}",
                              GetVkResultName(result));
    }
    support.Formats.resize(formatCount);

    std::uint32_t presentModeCount = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to read Vulkan present mode count: {}",
                              GetVkResultName(result));
    }

    support.PresentModes.resize(presentModeCount);
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice,
                                                       surface,
                                                       &presentModeCount,
                                                       support.PresentModes.data());
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to read Vulkan present modes: {}",
                              GetVkResultName(result));
    }
    support.PresentModes.resize(presentModeCount);

    return support;
}

class VulkanSwapchain::Impl final: public NCommon::NonTransferable {
public:
    Impl(const VulkanPhysicalDeviceSelection& physicalDevice,
         const VulkanDevice& device,
         VkSurfaceKHR surface,
         VkExtent2D framebufferExtent)
        : m_device(device.GetHandle())
        , m_config(MakeVulkanSwapchainConfig(ReadVulkanSwapchainSupport(physicalDevice.Handle, surface),
                                             framebufferExtent,
                                             physicalDevice.GraphicsQueueFamilyIndex,
                                             physicalDevice.PresentQueueFamilyIndex)) {
        if (m_config.Suspended) {
            return;
        }

        m_swapchain = CreateSwapchain(surface);
        try {
            m_images = ReadSwapchainImages(m_device, m_swapchain);
            m_imageViews.reserve(m_images.size());

            for (VkImage image: m_images) {
                m_imageViews.push_back(CreateImageView(m_device, image, m_config.ImageFormat));
            }
        } catch (...) {
            Destroy();
            throw;
        }
    }

    ~Impl() {
        Destroy();
    }

    void Destroy() noexcept {
        for (VkImageView imageView: m_imageViews) {
            vkDestroyImageView(m_device, imageView, nullptr);
        }
        m_imageViews.clear();

        if (m_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] bool IsSuspended() const noexcept {
        return m_config.Suspended;
    }

    [[nodiscard]] VkSwapchainKHR GetHandle() const noexcept {
        return m_swapchain;
    }

    [[nodiscard]] VkFormat GetImageFormat() const noexcept {
        return m_config.ImageFormat;
    }

    [[nodiscard]] VkExtent2D GetExtent() const noexcept {
        return m_config.Extent;
    }

    [[nodiscard]] std::span<const VkImage> GetImages() const noexcept {
        return m_images;
    }

    [[nodiscard]] std::span<const VkImageView> GetImageViews() const noexcept {
        return m_imageViews;
    }

    [[nodiscard]] std::optional<std::uint32_t>
    AcquireNextImage(std::uint64_t timeoutNanoseconds, VkSemaphore semaphore, VkFence fence) const {
        EnsureActive("Vulkan swapchain acquire requires an active swapchain");

        if (semaphore == VK_NULL_HANDLE && fence == VK_NULL_HANDLE) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                  "Vulkan swapchain acquire requires a semaphore or fence");
        }

        std::uint32_t imageIndex = 0;
        const VkResult result =
                vkAcquireNextImageKHR(m_device, m_swapchain, timeoutNanoseconds, semaphore, fence, &imageIndex);

        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            return imageIndex;
        }

        if (result == VK_NOT_READY || result == VK_TIMEOUT || result == VK_ERROR_OUT_OF_DATE_KHR) {
            return std::nullopt;
        }

        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to acquire Vulkan swapchain image: {}",
                              GetVkResultName(result));
    }

    void Present(VulkanLockedQueue presentQueue,
                 std::uint32_t imageIndex,
                 std::span<const VkSemaphore> waitSemaphores) const {
        EnsureActive("Vulkan swapchain present requires an active swapchain");

        const VkPresentInfoKHR presentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = nullptr,
                .waitSemaphoreCount = static_cast<std::uint32_t>(waitSemaphores.size()),
                .pWaitSemaphores = waitSemaphores.data(),
                .swapchainCount = 1,
                .pSwapchains = &m_swapchain,
                .pImageIndices = &imageIndex,
                .pResults = nullptr,
        };

        const VkResult result = vkQueuePresentKHR(presentQueue.GetHandle(), &presentInfo);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
            return;
        }

        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to present Vulkan swapchain image: {}",
                              GetVkResultName(result));
    }

private:
    [[nodiscard]] VkSwapchainKHR CreateSwapchain(VkSurfaceKHR surface) const {
        const VkSwapchainCreateInfoKHR createInfo{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                .pNext = nullptr,
                .flags = 0,
                .surface = surface,
                .minImageCount = m_config.ImageCount,
                .imageFormat = m_config.ImageFormat,
                .imageColorSpace = m_config.ColorSpace,
                .imageExtent = m_config.Extent,
                .imageArrayLayers = 1,
                .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                .imageSharingMode = m_config.SharingMode,
                .queueFamilyIndexCount = static_cast<std::uint32_t>(m_config.QueueFamilyIndices.size()),
                .pQueueFamilyIndices = m_config.QueueFamilyIndices.data(),
                .preTransform = m_config.PreTransform,
                .compositeAlpha = m_config.CompositeAlpha,
                .presentMode = m_config.PresentMode,
                .clipped = VK_TRUE,
                .oldSwapchain = VK_NULL_HANDLE,
        };

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        const VkResult result = vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &swapchain);
        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to create Vulkan swapchain: {}",
                                  GetVkResultName(result));
        }

        return swapchain;
    }

    void EnsureActive(std::string_view message) const {
        if (m_swapchain == VK_NULL_HANDLE) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "{}", message);
        }
    }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VulkanSwapchainConfig m_config;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_imageViews;
};

VulkanSwapchain::VulkanSwapchain(const VulkanPhysicalDeviceSelection& physicalDevice,
                                 const VulkanDevice& device,
                                 VkSurfaceKHR surface,
                                 VkExtent2D framebufferExtent)
    : m_impl(std::make_unique<Impl>(physicalDevice, device, surface, framebufferExtent)) {
}

VulkanSwapchain::~VulkanSwapchain() = default;

bool VulkanSwapchain::IsSuspended() const noexcept {
    return m_impl->IsSuspended();
}

VkSwapchainKHR VulkanSwapchain::GetHandle() const noexcept {
    return m_impl->GetHandle();
}

VkFormat VulkanSwapchain::GetImageFormat() const noexcept {
    return m_impl->GetImageFormat();
}

VkExtent2D VulkanSwapchain::GetExtent() const noexcept {
    return m_impl->GetExtent();
}

std::span<const VkImage> VulkanSwapchain::GetImages() const noexcept {
    return m_impl->GetImages();
}

std::span<const VkImageView> VulkanSwapchain::GetImageViews() const noexcept {
    return m_impl->GetImageViews();
}

std::optional<std::uint32_t>
VulkanSwapchain::AcquireNextImage(std::uint64_t timeoutNanoseconds, VkSemaphore semaphore, VkFence fence) const {
    return m_impl->AcquireNextImage(timeoutNanoseconds, semaphore, fence);
}

void VulkanSwapchain::Present(VulkanLockedQueue presentQueue,
                              std::uint32_t imageIndex,
                              std::span<const VkSemaphore> waitSemaphores) const {
    m_impl->Present(std::move(presentQueue), imageIndex, waitSemaphores);
}

} // namespace NVulkan
