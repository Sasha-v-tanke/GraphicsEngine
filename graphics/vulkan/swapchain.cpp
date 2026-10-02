#include "swapchain.h"

#include <algorithm>
#include <array>
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

constexpr VkFormat PreferredFormat = VK_FORMAT_B8G8R8A8_SRGB;
constexpr VkColorSpaceKHR PreferredColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
constexpr VkPresentModeKHR PreferredPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
constexpr VkImageUsageFlags RequiredImageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

std::string_view GetVkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
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
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

bool SupportsUsage(const VkSurfaceCapabilitiesKHR& capabilities) {
    return (capabilities.supportedUsageFlags & RequiredImageUsage) == RequiredImageUsage;
}

VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    const auto preferred = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& format) {
        return format.format == PreferredFormat && format.colorSpace == PreferredColorSpace;
    });

    if (preferred != formats.end()) {
        return *preferred;
    }

    return formats.front();
}

VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) {
    if (std::ranges::find(presentModes, PreferredPresentMode) != presentModes.end()) {
        return PreferredPresentMode;
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

std::uint32_t ClampDimension(int value, std::uint32_t minValue, std::uint32_t maxValue) {
    const std::uint32_t dimension = value <= 0 ? 0U : static_cast<std::uint32_t>(value);
    return std::clamp(dimension, minValue, maxValue);
}

VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, NWindow::WindowSize framebufferSize) {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    return {
            .width = ClampDimension(framebufferSize.Width,
                                    capabilities.minImageExtent.width,
                                    capabilities.maxImageExtent.width),
            .height = ClampDimension(framebufferSize.Height,
                                     capabilities.minImageExtent.height,
                                     capabilities.maxImageExtent.height),
    };
}

std::uint32_t ChooseImageCount(const VkSurfaceCapabilitiesKHR& capabilities) {
    std::uint32_t imageCount = capabilities.minImageCount + 1;

    if (capabilities.maxImageCount != 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    return imageCount;
}

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& capabilities) {
    constexpr std::array candidates{
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };

    for (VkCompositeAlphaFlagBitsKHR candidate: candidates) {
        if ((capabilities.supportedCompositeAlpha & candidate) == candidate) {
            return candidate;
        }
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Vulkan surface has no supported composite alpha mode");
}

VkSurfaceTransformFlagBitsKHR ChoosePreTransform(const VkSurfaceCapabilitiesKHR& capabilities) {
    if ((capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ==
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }

    return capabilities.currentTransform;
}

std::vector<std::uint32_t> MakeQueueFamilyIndices(std::uint32_t graphicsQueueFamilyIndex,
                                                  std::uint32_t presentQueueFamilyIndex) {
    if (graphicsQueueFamilyIndex == presentQueueFamilyIndex) {
        return {};
    }

    return {graphicsQueueFamilyIndex, presentQueueFamilyIndex};
}

void RequireSwapchainSupport(const VulkanSwapchainSupport& support) {
    if (support.Formats.empty()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Vulkan swapchain requires at least one surface format");
    }

    if (support.PresentModes.empty()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Vulkan swapchain requires at least one present mode");
    }

    if (std::ranges::find(support.PresentModes, VK_PRESENT_MODE_FIFO_KHR) == support.PresentModes.end()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Vulkan swapchain requires FIFO present mode support");
    }

    if (!SupportsUsage(support.Capabilities)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED,
                              "Vulkan swapchain surface does not support color attachment usage");
    }
}

std::vector<VkImage> ReadSwapchainImages(VkDevice device, VkSwapchainKHR swapchain) {
    while (true) {
        std::uint32_t count = 0;
        VkResult result = vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to enumerate Vulkan swapchain images: {}",
                                  GetVkResultName(result));
        }

        std::vector<VkImage> images(count);
        result = vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
        if (result == VK_INCOMPLETE) {
            continue;
        }

        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to read Vulkan swapchain images: {}",
                                  GetVkResultName(result));
        }

        images.resize(count);
        return images;
    }
}

VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format) {
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

    VkImageView imageView = VK_NULL_HANDLE;
    const VkResult result = vkCreateImageView(device, &createInfo, nullptr, &imageView);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to create Vulkan swapchain image view: {}",
                              GetVkResultName(result));
    }

    return imageView;
}

} // namespace

VulkanSwapchainSupport ReadVulkanSwapchainSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface) {
    if (physicalDevice == VK_NULL_HANDLE || surface == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan swapchain support requires device and surface");
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
                              "Failed to enumerate Vulkan surface formats: {}",
                              GetVkResultName(result));
    }

    support.Formats.resize(formatCount);
    if (formatCount != 0) {
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, support.Formats.data());
        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to read Vulkan surface formats: {}",
                                  GetVkResultName(result));
        }
        support.Formats.resize(formatCount);
    }

    std::uint32_t presentModeCount = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to enumerate Vulkan present modes: {}",
                              GetVkResultName(result));
    }

    support.PresentModes.resize(presentModeCount);
    if (presentModeCount != 0) {
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
    }

    return support;
}

VulkanSwapchainPlan MakeVulkanSwapchainPlan(const VulkanSwapchainSupport& support,
                                            NWindow::WindowSize framebufferSize,
                                            std::uint32_t graphicsQueueFamilyIndex,
                                            std::uint32_t presentQueueFamilyIndex) {
    if (framebufferSize.Width <= 0 || framebufferSize.Height <= 0) {
        return {
                .Suspended = true,
        };
    }

    RequireSwapchainSupport(support);

    const VkExtent2D extent = ChooseExtent(support.Capabilities, framebufferSize);
    if (extent.width == 0 || extent.height == 0) {
        return {
                .Suspended = true,
        };
    }

    const std::vector<std::uint32_t> queueFamilyIndices =
            MakeQueueFamilyIndices(graphicsQueueFamilyIndex, presentQueueFamilyIndex);

    return {
            .Suspended = false,
            .SurfaceFormat = ChooseSurfaceFormat(support.Formats),
            .PresentMode = ChoosePresentMode(support.PresentModes),
            .Extent = extent,
            .ImageCount = ChooseImageCount(support.Capabilities),
            .SharingMode = queueFamilyIndices.empty() ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
            .PreTransform = ChoosePreTransform(support.Capabilities),
            .CompositeAlpha = ChooseCompositeAlpha(support.Capabilities),
            .QueueFamilyIndices = queueFamilyIndices,
    };
}

class VulkanSwapchain::Impl final: public NCommon::NonTransferable {
public:
    Impl(const VulkanDevice& device,
         const VulkanPhysicalDeviceSelection& physicalDevice,
         VkSurfaceKHR surface,
         NWindow::WindowSize framebufferSize)
        : m_device(device.GetHandle())
        , m_plan(MakeVulkanSwapchainPlan(ReadVulkanSwapchainSupport(physicalDevice.Handle, surface),
                                         framebufferSize,
                                         physicalDevice.GraphicsQueueFamilyIndex,
                                         physicalDevice.PresentQueueFamilyIndex)) {
        if (m_plan.Suspended) {
            return;
        }

        try {
            m_swapchain = CreateSwapchain(surface);
            m_images = CreateImages();
        } catch (...) {
            Destroy();
            throw;
        }
    }

    ~Impl() {
        Destroy();
    }

    void Destroy() noexcept {
        for (const VulkanSwapchainImage& image: m_images) {
            if (image.View != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, image.View, nullptr);
            }
        }
        m_images.clear();

        if (m_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] bool IsSuspended() const noexcept {
        return m_plan.Suspended;
    }

    [[nodiscard]] const VulkanSwapchainPlan& GetPlan() const noexcept {
        return m_plan;
    }

    [[nodiscard]] VkSwapchainKHR GetHandle() const noexcept {
        return m_swapchain;
    }

    [[nodiscard]] const std::vector<VulkanSwapchainImage>& GetImages() const noexcept {
        return m_images;
    }

    [[nodiscard]] VulkanSwapchainAcquireResult
    AcquireNextImage(VkSemaphore signalSemaphore, VkFence signalFence, std::uint64_t timeoutNanoseconds) const {
        if (m_plan.Suspended) {
            return {
                    .Status = EVulkanSwapchainAcquireStatus::SUSPENDED,
            };
        }

        if (signalSemaphore == VK_NULL_HANDLE && signalFence == VK_NULL_HANDLE) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                  "Vulkan swapchain acquire requires a semaphore or fence");
        }

        std::uint32_t imageIndex = 0;
        const VkResult result = vkAcquireNextImageKHR(m_device,
                                                      m_swapchain,
                                                      timeoutNanoseconds,
                                                      signalSemaphore,
                                                      signalFence,
                                                      &imageIndex);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            return {
                    .Status = EVulkanSwapchainAcquireStatus::ACQUIRED,
                    .ImageIndex = imageIndex,
            };
        }

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            return {
                    .Status = EVulkanSwapchainAcquireStatus::OUT_OF_DATE,
            };
        }

        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to acquire Vulkan swapchain image: {}",
                              GetVkResultName(result));
    }

    [[nodiscard]] EVulkanSwapchainPresentStatus Present(const VulkanLockedQueue& presentQueue,
                                                        std::uint32_t imageIndex,
                                                        const std::vector<VkSemaphore>& waitSemaphores) const {
        if (m_plan.Suspended) {
            return EVulkanSwapchainPresentStatus::SUSPENDED;
        }

        if (imageIndex >= m_images.size()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan swapchain image index is out of range");
        }

        const VkSwapchainKHR swapchain = m_swapchain;
        const VkPresentInfoKHR presentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = nullptr,
                .waitSemaphoreCount = static_cast<std::uint32_t>(waitSemaphores.size()),
                .pWaitSemaphores = waitSemaphores.data(),
                .swapchainCount = 1,
                .pSwapchains = &swapchain,
                .pImageIndices = &imageIndex,
                .pResults = nullptr,
        };

        const VkResult result = vkQueuePresentKHR(presentQueue.GetHandle(), &presentInfo);
        if (result == VK_SUCCESS) {
            return EVulkanSwapchainPresentStatus::PRESENTED;
        }

        if (result == VK_SUBOPTIMAL_KHR) {
            return EVulkanSwapchainPresentStatus::SUBOPTIMAL;
        }

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            return EVulkanSwapchainPresentStatus::OUT_OF_DATE;
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
                .minImageCount = m_plan.ImageCount,
                .imageFormat = m_plan.SurfaceFormat.format,
                .imageColorSpace = m_plan.SurfaceFormat.colorSpace,
                .imageExtent = m_plan.Extent,
                .imageArrayLayers = 1,
                .imageUsage = RequiredImageUsage,
                .imageSharingMode = m_plan.SharingMode,
                .queueFamilyIndexCount = static_cast<std::uint32_t>(m_plan.QueueFamilyIndices.size()),
                .pQueueFamilyIndices = m_plan.QueueFamilyIndices.empty() ? nullptr : m_plan.QueueFamilyIndices.data(),
                .preTransform = m_plan.PreTransform,
                .compositeAlpha = m_plan.CompositeAlpha,
                .presentMode = m_plan.PresentMode,
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

    [[nodiscard]] std::vector<VulkanSwapchainImage> CreateImages() const {
        const std::vector<VkImage> images = ReadSwapchainImages(m_device, m_swapchain);
        std::vector<VulkanSwapchainImage> swapchainImages;
        swapchainImages.reserve(images.size());

        try {
            for (std::uint32_t index = 0; index < images.size(); ++index) {
                swapchainImages.push_back({
                        .Index = index,
                        .Handle = images[index],
                        .View = CreateImageView(m_device, images[index], m_plan.SurfaceFormat.format),
                });
            }
        } catch (...) {
            for (const VulkanSwapchainImage& image: swapchainImages) {
                if (image.View != VK_NULL_HANDLE) {
                    vkDestroyImageView(m_device, image.View, nullptr);
                }
            }
            throw;
        }

        return swapchainImages;
    }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VulkanSwapchainPlan m_plan;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VulkanSwapchainImage> m_images;
};

VulkanSwapchain::VulkanSwapchain(const VulkanDevice& device,
                                 const VulkanPhysicalDeviceSelection& physicalDevice,
                                 VkSurfaceKHR surface,
                                 NWindow::WindowSize framebufferSize)
    : m_impl(std::make_unique<Impl>(device, physicalDevice, surface, framebufferSize)) {
}

VulkanSwapchain::~VulkanSwapchain() = default;

bool VulkanSwapchain::IsSuspended() const noexcept {
    return m_impl->IsSuspended();
}

const VulkanSwapchainPlan& VulkanSwapchain::GetPlan() const noexcept {
    return m_impl->GetPlan();
}

VkSwapchainKHR VulkanSwapchain::GetHandle() const noexcept {
    return m_impl->GetHandle();
}

const std::vector<VulkanSwapchainImage>& VulkanSwapchain::GetImages() const noexcept {
    return m_impl->GetImages();
}

VulkanSwapchainAcquireResult VulkanSwapchain::AcquireNextImage(VkSemaphore signalSemaphore,
                                                               VkFence signalFence,
                                                               std::uint64_t timeoutNanoseconds) const {
    return m_impl->AcquireNextImage(signalSemaphore, signalFence, timeoutNanoseconds);
}

EVulkanSwapchainPresentStatus VulkanSwapchain::Present(const VulkanLockedQueue& presentQueue,
                                                       std::uint32_t imageIndex,
                                                       const std::vector<VkSemaphore>& waitSemaphores) const {
    return m_impl->Present(presentQueue, imageIndex, waitSemaphores);
}

} // namespace NVulkan
