#include "device.h"
#include "resource_conversion.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

namespace {

constexpr float QueuePriority = 1.0F;
constexpr std::uint32_t VulkanMaxFramesInFlight = 2;
constexpr std::string_view PortabilitySubsetExtension = "VK_KHR_portability_subset";

std::string_view GetVkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

bool ContainsQueueFamily(const std::vector<VulkanDeviceQueuePlan>& queueFamilies, std::uint32_t familyIndex) {
    return std::ranges::any_of(queueFamilies, [familyIndex](const VulkanDeviceQueuePlan& queueFamily) {
        return queueFamily.FamilyIndex == familyIndex;
    });
}

bool ContainsExtension(const std::vector<std::string>& extensions, std::string_view extension) {
    return std::ranges::find(extensions, extension) != extensions.end();
}

void AppendQueueFamily(std::vector<VulkanDeviceQueuePlan>& queueFamilies, std::uint32_t familyIndex) {
    if (ContainsQueueFamily(queueFamilies, familyIndex)) {
        return;
    }

    queueFamilies.push_back({
            .FamilyIndex = familyIndex,
    });
}

std::vector<const char*> MakeNamePointers(const std::vector<std::string>& values) {
    std::vector<const char*> names;
    names.reserve(values.size());

    for (const std::string& value: values) {
        names.push_back(value.c_str());
    }

    return names;
}

std::vector<VkDeviceQueueCreateInfo> MakeQueueCreateInfos(const std::vector<VulkanDeviceQueuePlan>& queueFamilies) {
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(queueFamilies.size());

    for (const VulkanDeviceQueuePlan& queueFamily: queueFamilies) {
        queueCreateInfos.push_back({
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queueFamilyIndex = queueFamily.FamilyIndex,
                .queueCount = 1,
                .pQueuePriorities = &QueuePriority,
        });
    }

    return queueCreateInfos;
}

NGraphics::GraphicsCapabilities MakeGraphicsCapabilities(const VulkanPhysicalDeviceCapabilities& capabilities) {
    return {
            .Presentation = capabilities.SurfaceSupported && capabilities.SwapchainFormatsSupported &&
                            capabilities.SwapchainPresentModesSupported,
            .TimelineCompletion = capabilities.Features.TimelineSemaphore,
            .MaxFramesInFlight = VulkanMaxFramesInFlight,
    };
}

std::vector<std::string> MakeDeviceExtensions(const VulkanPhysicalDeviceCapabilities& capabilities) {
    std::vector<std::string> extensions{
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    if (ContainsExtension(capabilities.Extensions, PortabilitySubsetExtension)) {
        extensions.emplace_back(PortabilitySubsetExtension);
    }

    return extensions;
}

} // namespace

VulkanLockedQueue::VulkanLockedQueue(std::unique_lock<std::mutex> lock,
                                     VkQueue handle,
                                     std::uint32_t familyIndex) noexcept
    : m_lock(std::move(lock))
    , m_handle(handle)
    , m_familyIndex(familyIndex) {
}

VkQueue VulkanLockedQueue::GetHandle() const noexcept {
    return m_handle;
}

std::uint32_t VulkanLockedQueue::GetFamilyIndex() const noexcept {
    return m_familyIndex;
}

VulkanDevicePlan MakeVulkanDevicePlan(const VulkanPhysicalDeviceSelection& physicalDevice) {
    if (physicalDevice.Handle == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan device requires a physical device");
    }

    std::vector<VulkanDeviceQueuePlan> queueFamilies;
    AppendQueueFamily(queueFamilies, physicalDevice.GraphicsQueueFamilyIndex);
    AppendQueueFamily(queueFamilies, physicalDevice.PresentQueueFamilyIndex);

    return {
            .QueueFamilies = std::move(queueFamilies),
            .Extensions = MakeDeviceExtensions(physicalDevice.Capabilities),
            .Features = physicalDevice.Capabilities.Features,
            .GraphicsCapabilities = MakeGraphicsCapabilities(physicalDevice.Capabilities),
    };
}

class VulkanDevice::Impl final: public NCommon::NonTransferable {
public:
    explicit Impl(const VulkanPhysicalDeviceSelection& physicalDevice)
        : m_physicalDevice(physicalDevice.Handle)
        , m_plan(MakeVulkanDevicePlan(physicalDevice))
        , m_graphicsQueueFamilyIndex(physicalDevice.GraphicsQueueFamilyIndex)
        , m_presentQueueFamilyIndex(physicalDevice.PresentQueueFamilyIndex)
        , m_device(CreateDevice()) {
        try {
            volkLoadDevice(m_device);
            m_graphicsQueue = ReadQueue(m_graphicsQueueFamilyIndex);
            m_presentQueue = ReadQueue(m_presentQueueFamilyIndex);
        } catch (...) {
            Destroy();
            throw;
        }
    }

    ~Impl() {
        Destroy();
    }

    void Destroy() noexcept {
        if (m_device != VK_NULL_HANDLE) {
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] VkDevice GetHandle() const noexcept {
        return m_device;
    }

    [[nodiscard]] const NGraphics::GraphicsCapabilities& GetGraphicsCapabilities() const noexcept {
        return m_plan.GraphicsCapabilities;
    }

    [[nodiscard]] VulkanLockedQueue LockGraphicsQueue() const {
        return VulkanLockedQueue{std::unique_lock{m_queueMutex}, m_graphicsQueue, m_graphicsQueueFamilyIndex};
    }

    [[nodiscard]] VulkanLockedQueue LockPresentQueue() const {
        return VulkanLockedQueue{std::unique_lock{m_queueMutex}, m_presentQueue, m_presentQueueFamilyIndex};
    }

    [[nodiscard]] VkImage CreateImage(const NGraphics::ImageDescriptor& descriptor) const {
        const VkFormat format = ToVulkanFormat(descriptor.Format);
        const VkImageUsageFlags usage = ToVulkanImageUsage(descriptor.Usage);
        ValidateImageSupport(descriptor, format, usage);

        const VkImageCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = format,
                .extent =
                        {
                                .width = descriptor.Extent.Width,
                                .height = descriptor.Extent.Height,
                                .depth = descriptor.Extent.Depth,
                        },
                .mipLevels = descriptor.MipLevels,
                .arrayLayers = descriptor.ArrayLayers,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VkImage image = VK_NULL_HANDLE;
        const VkResult result = vkCreateImage(m_device, &createInfo, nullptr, &image);
        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to create Vulkan image: {}",
                                  GetVkResultName(result));
        }

        return image;
    }

    void DestroyImage(VkImage image) const noexcept {
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, image, nullptr);
        }
    }

    [[nodiscard]] VkImageView CreateImageView(VkImage image, const NGraphics::ImageViewDescriptor& descriptor) const {
        if (image == VK_NULL_HANDLE) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan image view requires an image");
        }

        const VkImageViewCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = ToVulkanFormat(descriptor.Format),
                .components =
                        {
                                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
                        },
                .subresourceRange =
                        {
                                .aspectMask = ToVulkanImageAspect(descriptor.Aspects),
                                .baseMipLevel = descriptor.BaseMipLevel,
                                .levelCount = descriptor.LevelCount,
                                .baseArrayLayer = descriptor.BaseArrayLayer,
                                .layerCount = descriptor.LayerCount,
                        },
        };

        VkImageView imageView = VK_NULL_HANDLE;
        const VkResult result = vkCreateImageView(m_device, &createInfo, nullptr, &imageView);
        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to create Vulkan image view: {}",
                                  GetVkResultName(result));
        }

        return imageView;
    }

    void DestroyImageView(VkImageView imageView) const noexcept {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, imageView, nullptr);
        }
    }

    [[nodiscard]] VkSampler CreateSampler(const NGraphics::SamplerDescriptor& descriptor) const {
        const VkSamplerCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .magFilter = ToVulkanFilter(descriptor.MagFilter),
                .minFilter = ToVulkanFilter(descriptor.MinFilter),
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                .addressModeU = ToVulkanAddressMode(descriptor.AddressModeU),
                .addressModeV = ToVulkanAddressMode(descriptor.AddressModeV),
                .addressModeW = ToVulkanAddressMode(descriptor.AddressModeW),
                .mipLodBias = 0.0F,
                .anisotropyEnable = VK_FALSE,
                .maxAnisotropy = 1.0F,
                .compareEnable = VK_FALSE,
                .compareOp = VK_COMPARE_OP_ALWAYS,
                .minLod = descriptor.MinLod,
                .maxLod = descriptor.MaxLod,
                .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                .unnormalizedCoordinates = VK_FALSE,
        };

        VkSampler sampler = VK_NULL_HANDLE;
        const VkResult result = vkCreateSampler(m_device, &createInfo, nullptr, &sampler);
        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to create Vulkan sampler: {}",
                                  GetVkResultName(result));
        }

        return sampler;
    }

    void DestroySampler(VkSampler sampler) const noexcept {
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device, sampler, nullptr);
        }
    }

private:
    [[nodiscard]] VkDevice CreateDevice() const {
        const std::vector<const char*> extensionNames = MakeNamePointers(m_plan.Extensions);
        const std::vector<VkDeviceQueueCreateInfo> queueCreateInfos = MakeQueueCreateInfos(m_plan.QueueFamilies);

        VkPhysicalDeviceVulkan13Features features13{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                .pNext = nullptr,
                .synchronization2 = m_plan.Features.Synchronization2 ? VK_TRUE : VK_FALSE,
                .dynamicRendering = m_plan.Features.DynamicRendering ? VK_TRUE : VK_FALSE,
        };
        VkPhysicalDeviceVulkan12Features features12{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                .pNext = &features13,
                .timelineSemaphore = m_plan.Features.TimelineSemaphore ? VK_TRUE : VK_FALSE,
        };

        const VkDeviceCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .pNext = &features12,
                .flags = 0,
                .queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size()),
                .pQueueCreateInfos = queueCreateInfos.data(),
                .enabledLayerCount = 0,
                .ppEnabledLayerNames = nullptr,
                .enabledExtensionCount = static_cast<std::uint32_t>(extensionNames.size()),
                .ppEnabledExtensionNames = extensionNames.data(),
                .pEnabledFeatures = nullptr,
        };

        VkDevice device = VK_NULL_HANDLE;
        const VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &device);
        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to create Vulkan logical device: {}",
                                  GetVkResultName(result));
        }

        return device;
    }

    [[nodiscard]] VkQueue ReadQueue(std::uint32_t familyIndex) const {
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(m_device, familyIndex, 0, &queue);

        if (queue == VK_NULL_HANDLE) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Vulkan queue handle is null");
        }

        return queue;
    }

    void
    ValidateImageSupport(const NGraphics::ImageDescriptor& descriptor, VkFormat format, VkImageUsageFlags usage) const {
        const VkPhysicalDeviceImageFormatInfo2 formatInfo{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                .pNext = nullptr,
                .format = format,
                .type = VK_IMAGE_TYPE_2D,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = usage,
                .flags = 0,
        };

        VkImageFormatProperties2 properties{
                .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                .pNext = nullptr,
                .imageFormatProperties = {},
        };
        const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(m_physicalDevice, &formatInfo, &properties);

        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED,
                                  "Vulkan image format and usage combination is not supported");
        }

        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to query Vulkan image format support: {}",
                                  GetVkResultName(result));
        }

        const VkImageFormatProperties& imageProperties = properties.imageFormatProperties;
        if (descriptor.Extent.Width > imageProperties.maxExtent.width ||
            descriptor.Extent.Height > imageProperties.maxExtent.height ||
            descriptor.Extent.Depth > imageProperties.maxExtent.depth ||
            descriptor.MipLevels > imageProperties.maxMipLevels ||
            descriptor.ArrayLayers > imageProperties.maxArrayLayers ||
            (imageProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED,
                                  "Vulkan image descriptor exceeds supported image format limits");
        }
    }

private:
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VulkanDevicePlan m_plan;
    std::uint32_t m_graphicsQueueFamilyIndex = 0;
    std::uint32_t m_presentQueueFamilyIndex = 0;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    mutable std::mutex m_queueMutex;
};

VulkanDevice::VulkanDevice(const VulkanPhysicalDeviceSelection& physicalDevice)
    : m_impl(std::make_unique<Impl>(physicalDevice)) {
}

VulkanDevice::~VulkanDevice() = default;

VkDevice VulkanDevice::GetHandle() const noexcept {
    return m_impl->GetHandle();
}

const NGraphics::GraphicsCapabilities& VulkanDevice::GetGraphicsCapabilities() const noexcept {
    return m_impl->GetGraphicsCapabilities();
}

VulkanLockedQueue VulkanDevice::LockGraphicsQueue() const {
    return m_impl->LockGraphicsQueue();
}

VulkanLockedQueue VulkanDevice::LockPresentQueue() const {
    return m_impl->LockPresentQueue();
}

VkImage VulkanDevice::CreateImage(const NGraphics::ImageDescriptor& descriptor) const {
    return m_impl->CreateImage(descriptor);
}

void VulkanDevice::DestroyImage(VkImage image) const noexcept {
    m_impl->DestroyImage(image);
}

VkImageView VulkanDevice::CreateImageView(VkImage image, const NGraphics::ImageViewDescriptor& descriptor) const {
    return m_impl->CreateImageView(image, descriptor);
}

void VulkanDevice::DestroyImageView(VkImageView imageView) const noexcept {
    m_impl->DestroyImageView(imageView);
}

VkSampler VulkanDevice::CreateSampler(const NGraphics::SamplerDescriptor& descriptor) const {
    return m_impl->CreateSampler(descriptor);
}

void VulkanDevice::DestroySampler(VkSampler sampler) const noexcept {
    m_impl->DestroySampler(sampler);
}

} // namespace NVulkan
