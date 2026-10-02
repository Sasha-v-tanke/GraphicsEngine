#include "device.h"

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

} // namespace NVulkan
