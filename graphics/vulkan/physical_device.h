#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <vulkan.h>

#include <GraphicsEngine/graphics/vulkan/instance.h>

namespace NVulkan {

enum class EVulkanPhysicalDeviceType {
    OTHER,
    INTEGRATED_GPU,
    DISCRETE_GPU,
    VIRTUAL_GPU,
    CPU,
};

struct VulkanPhysicalDeviceFeatures {
    bool TimelineSemaphore = false;
    bool Synchronization2 = false;
    bool DynamicRendering = false;
};

struct VulkanQueueFamilyCapabilities {
    std::uint32_t Index = 0;
    std::uint32_t QueueCount = 0;
    bool Graphics = false;
    bool Present = false;
};

struct VulkanPhysicalDeviceCapabilities {
    std::string Name;
    std::uint32_t ApiVersion = VK_API_VERSION_1_0;
    std::uint32_t VendorId = 0;
    std::uint32_t DeviceId = 0;
    std::array<std::uint8_t, VK_UUID_SIZE> DeviceUuid{};
    EVulkanPhysicalDeviceType Type = EVulkanPhysicalDeviceType::OTHER;
    std::uint32_t MaxImageDimension2D = 0;
    std::vector<std::string> Extensions;
    VulkanPhysicalDeviceFeatures Features;
    std::vector<VulkanQueueFamilyCapabilities> QueueFamilies;
    bool SurfaceSupported = false;
    bool SwapchainFormatsSupported = false;
    bool SwapchainPresentModesSupported = false;
};

struct VulkanPhysicalDeviceRequirements {
    std::uint32_t RequiredApiVersion = VK_API_VERSION_1_3;
    std::vector<std::string> RequiredExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VulkanPhysicalDeviceFeatures RequiredFeatures{
            .TimelineSemaphore = true,
            .Synchronization2 = true,
            .DynamicRendering = true,
    };
};

struct VulkanPhysicalDeviceEvaluation {
    bool Suitable = false;
    std::uint64_t Score = 0;
    std::optional<std::uint32_t> GraphicsQueueFamilyIndex;
    std::optional<std::uint32_t> PresentQueueFamilyIndex;
    std::vector<std::string> RejectionReasons;
};

struct VulkanPhysicalDeviceSelectionPlan {
    std::size_t CandidateIndex = 0;
    std::uint64_t Score = 0;
    std::uint32_t GraphicsQueueFamilyIndex = 0;
    std::uint32_t PresentQueueFamilyIndex = 0;
};

struct VulkanPhysicalDeviceSelection {
    VkPhysicalDevice Handle = VK_NULL_HANDLE;
    VulkanPhysicalDeviceCapabilities Capabilities;
    std::uint32_t GraphicsQueueFamilyIndex = 0;
    std::uint32_t PresentQueueFamilyIndex = 0;
};

[[nodiscard]] VulkanPhysicalDeviceEvaluation
EvaluateVulkanPhysicalDevice(const VulkanPhysicalDeviceCapabilities& capabilities,
                             const VulkanPhysicalDeviceRequirements& requirements = {});

[[nodiscard]] VulkanPhysicalDeviceSelectionPlan
MakeVulkanPhysicalDeviceSelectionPlan(const std::vector<VulkanPhysicalDeviceCapabilities>& candidates,
                                      const VulkanPhysicalDeviceRequirements& requirements = {});

[[nodiscard]] VulkanPhysicalDeviceSelection
SelectVulkanPhysicalDevice(const VulkanInstance& instance,
                           VkSurfaceKHR surface,
                           const VulkanPhysicalDeviceRequirements& requirements = {});

} // namespace NVulkan
