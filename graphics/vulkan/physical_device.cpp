#include "physical_device.h"

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

namespace {

constexpr std::uint64_t OTHER_DEVICE_SCORE = 1'000'000;
constexpr std::uint64_t CPU_DEVICE_SCORE = 2'000'000;
constexpr std::uint64_t VIRTUAL_DEVICE_SCORE = 3'000'000;
constexpr std::uint64_t INTEGRATED_DEVICE_SCORE = 4'000'000;
constexpr std::uint64_t DISCRETE_DEVICE_SCORE = 5'000'000;
constexpr std::uint64_t SHARED_QUEUE_FAMILY_SCORE = 100'000;

std::string_view GetVkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

bool Contains(const std::vector<std::string>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

std::string FormatApiVersion(std::uint32_t version) {
    return std::format("{}.{}.{}",
                       VK_API_VERSION_MAJOR(version),
                       VK_API_VERSION_MINOR(version),
                       VK_API_VERSION_PATCH(version));
}

std::optional<std::uint32_t> FindQueueFamily(const std::vector<VulkanQueueFamilyCapabilities>& queueFamilies,
                                             bool requireGraphics,
                                             bool requirePresent) {
    std::optional<std::uint32_t> selected;

    for (const VulkanQueueFamilyCapabilities& queueFamily: queueFamilies) {
        if (queueFamily.QueueCount == 0 || (requireGraphics && !queueFamily.Graphics) ||
            (requirePresent && !queueFamily.Present)) {
            continue;
        }

        if (!selected.has_value() || queueFamily.Index < *selected) {
            selected = queueFamily.Index;
        }
    }

    return selected;
}

std::uint64_t GetDeviceTypeScore(EVulkanPhysicalDeviceType type) {
    switch (type) {
    case EVulkanPhysicalDeviceType::OTHER:
        return OTHER_DEVICE_SCORE;
    case EVulkanPhysicalDeviceType::INTEGRATED_GPU:
        return INTEGRATED_DEVICE_SCORE;
    case EVulkanPhysicalDeviceType::DISCRETE_GPU:
        return DISCRETE_DEVICE_SCORE;
    case EVulkanPhysicalDeviceType::VIRTUAL_GPU:
        return VIRTUAL_DEVICE_SCORE;
    case EVulkanPhysicalDeviceType::CPU:
        return CPU_DEVICE_SCORE;
    }

    return 0;
}

std::uint64_t ScoreDevice(const VulkanPhysicalDeviceCapabilities& capabilities,
                          const VulkanPhysicalDeviceEvaluation& evaluation) {
    std::uint64_t score = GetDeviceTypeScore(capabilities.Type) + capabilities.MaxImageDimension2D;

    if (evaluation.GraphicsQueueFamilyIndex == evaluation.PresentQueueFamilyIndex) {
        score += SHARED_QUEUE_FAMILY_SCORE;
    }

    return score;
}

bool IsPreferredCandidate(const VulkanPhysicalDeviceCapabilities& candidate,
                          const VulkanPhysicalDeviceEvaluation& candidateEvaluation,
                          const VulkanPhysicalDeviceCapabilities& current,
                          const VulkanPhysicalDeviceEvaluation& currentEvaluation) {
    if (candidateEvaluation.Score != currentEvaluation.Score) {
        return candidateEvaluation.Score > currentEvaluation.Score;
    }

    if (candidate.DeviceUuid != current.DeviceUuid) {
        return candidate.DeviceUuid < current.DeviceUuid;
    }

    if (candidate.VendorId != current.VendorId) {
        return candidate.VendorId < current.VendorId;
    }

    if (candidate.DeviceId != current.DeviceId) {
        return candidate.DeviceId < current.DeviceId;
    }

    return candidate.Name < current.Name;
}

std::string JoinReasons(const std::vector<std::string>& reasons) {
    std::string result;

    for (const std::string& reason: reasons) {
        if (!result.empty()) {
            result += ", ";
        }

        result += reason;
    }

    return result;
}

EVulkanPhysicalDeviceType ConvertDeviceType(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return EVulkanPhysicalDeviceType::INTEGRATED_GPU;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return EVulkanPhysicalDeviceType::DISCRETE_GPU;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return EVulkanPhysicalDeviceType::VIRTUAL_GPU;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return EVulkanPhysicalDeviceType::CPU;
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
    case VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM:
        return EVulkanPhysicalDeviceType::OTHER;
    }

    return EVulkanPhysicalDeviceType::OTHER;
}

std::vector<std::string> EnumerateDeviceExtensions(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to enumerate Vulkan device extensions: {}",
                              GetVkResultName(result));
    }

    std::vector<VkExtensionProperties> properties(count);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data());
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to read Vulkan device extensions: {}",
                              GetVkResultName(result));
    }

    std::vector<std::string> extensions;
    extensions.reserve(count);

    for (const VkExtensionProperties& property: properties) {
        extensions.emplace_back(property.extensionName);
    }

    return extensions;
}

VulkanPhysicalDeviceFeatures ReadDeviceFeatures(VkPhysicalDevice device) {
    VkPhysicalDeviceVulkan13Features features13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features features12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext = &features13,
    };
    VkPhysicalDeviceFeatures2 features2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &features12,
    };

    vkGetPhysicalDeviceFeatures2(device, &features2);

    return {
            .TimelineSemaphore = features12.timelineSemaphore == VK_TRUE,
            .Synchronization2 = features13.synchronization2 == VK_TRUE,
            .DynamicRendering = features13.dynamicRendering == VK_TRUE,
    };
}

std::vector<VulkanQueueFamilyCapabilities> ReadQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);

    std::vector<VkQueueFamilyProperties> properties(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());

    std::vector<VulkanQueueFamilyCapabilities> queueFamilies;
    queueFamilies.reserve(count);

    for (std::uint32_t index = 0; index < count; ++index) {
        VkBool32 presentSupported = VK_FALSE;
        const VkResult presentResult = vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface, &presentSupported);

        queueFamilies.push_back({
                .Index = index,
                .QueueCount = properties[index].queueCount,
                .Graphics = (properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0,
                .Present = presentResult == VK_SUCCESS && presentSupported == VK_TRUE,
        });
    }

    return queueFamilies;
}

void ReadSwapchainSupport(VkPhysicalDevice device,
                          VkSurfaceKHR surface,
                          VulkanPhysicalDeviceCapabilities& capabilities) {
    VkSurfaceCapabilitiesKHR surfaceCapabilities{};
    const VkResult surfaceResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &surfaceCapabilities);
    capabilities.SurfaceSupported = surfaceResult == VK_SUCCESS;
    if (!capabilities.SurfaceSupported) {
        return;
    }

    std::uint32_t formatCount = 0;
    const VkResult formatResult = vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    capabilities.SwapchainFormatsSupported = formatResult == VK_SUCCESS && formatCount > 0;

    std::uint32_t presentModeCount = 0;
    const VkResult presentModeResult =
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    capabilities.SwapchainPresentModesSupported = presentModeResult == VK_SUCCESS && presentModeCount > 0;
}

VulkanPhysicalDeviceCapabilities ReadDeviceCapabilities(VkPhysicalDevice device, VkSurfaceKHR surface) {
    VkPhysicalDeviceIDProperties idProperties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 properties2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &idProperties,
    };
    vkGetPhysicalDeviceProperties2(device, &properties2);

    const VkPhysicalDeviceProperties& properties = properties2.properties;

    VulkanPhysicalDeviceCapabilities capabilities{
            .Name = properties.deviceName,
            .ApiVersion = properties.apiVersion,
            .VendorId = properties.vendorID,
            .DeviceId = properties.deviceID,
            .Type = ConvertDeviceType(properties.deviceType),
            .MaxImageDimension2D = properties.limits.maxImageDimension2D,
            .Extensions = EnumerateDeviceExtensions(device),
            .Features = ReadDeviceFeatures(device),
            .QueueFamilies = ReadQueueFamilies(device, surface),
    };
    std::ranges::copy(idProperties.deviceUUID, capabilities.DeviceUuid.begin());
    ReadSwapchainSupport(device, surface, capabilities);

    return capabilities;
}

std::vector<VkPhysicalDevice> EnumeratePhysicalDevices(VkInstance instance) {
    std::uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to enumerate Vulkan physical devices: {}",
                              GetVkResultName(result));
    }

    if (count == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_FOUND, "No Vulkan physical devices are available");
    }

    std::vector<VkPhysicalDevice> devices(count);
    result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to read Vulkan physical devices: {}",
                              GetVkResultName(result));
    }

    return devices;
}

} // namespace

VulkanPhysicalDeviceEvaluation EvaluateVulkanPhysicalDevice(const VulkanPhysicalDeviceCapabilities& capabilities,
                                                            const VulkanPhysicalDeviceRequirements& requirements) {
    VulkanPhysicalDeviceEvaluation evaluation;

    if (capabilities.ApiVersion < requirements.RequiredApiVersion) {
        evaluation.RejectionReasons.push_back(std::format("API version {} is below required {}",
                                                          FormatApiVersion(capabilities.ApiVersion),
                                                          FormatApiVersion(requirements.RequiredApiVersion)));
    }

    for (const std::string& extension: requirements.RequiredExtensions) {
        if (!Contains(capabilities.Extensions, extension)) {
            evaluation.RejectionReasons.push_back(std::format("required extension '{}' is not supported", extension));
        }
    }

    if (requirements.RequiredFeatures.TimelineSemaphore && !capabilities.Features.TimelineSemaphore) {
        evaluation.RejectionReasons.emplace_back("required feature timelineSemaphore is not supported");
    }

    if (requirements.RequiredFeatures.Synchronization2 && !capabilities.Features.Synchronization2) {
        evaluation.RejectionReasons.emplace_back("required feature synchronization2 is not supported");
    }

    if (requirements.RequiredFeatures.DynamicRendering && !capabilities.Features.DynamicRendering) {
        evaluation.RejectionReasons.emplace_back("required feature dynamicRendering is not supported");
    }

    const std::optional<std::uint32_t> sharedQueue = FindQueueFamily(capabilities.QueueFamilies, true, true);
    if (sharedQueue.has_value()) {
        evaluation.GraphicsQueueFamilyIndex = sharedQueue;
        evaluation.PresentQueueFamilyIndex = sharedQueue;
    } else {
        evaluation.GraphicsQueueFamilyIndex = FindQueueFamily(capabilities.QueueFamilies, true, false);
        evaluation.PresentQueueFamilyIndex = FindQueueFamily(capabilities.QueueFamilies, false, true);
    }

    if (!evaluation.GraphicsQueueFamilyIndex.has_value()) {
        evaluation.RejectionReasons.emplace_back("graphics queue is not available");
    }

    if (!evaluation.PresentQueueFamilyIndex.has_value()) {
        evaluation.RejectionReasons.emplace_back("present queue is not available");
    }

    if (!capabilities.SurfaceSupported) {
        evaluation.RejectionReasons.emplace_back("surface is not supported");
    }

    if (!capabilities.SwapchainFormatsSupported) {
        evaluation.RejectionReasons.emplace_back("swapchain formats are not available");
    }

    if (!capabilities.SwapchainPresentModesSupported) {
        evaluation.RejectionReasons.emplace_back("swapchain present modes are not available");
    }

    evaluation.Suitable = evaluation.RejectionReasons.empty();
    if (evaluation.Suitable) {
        evaluation.Score = ScoreDevice(capabilities, evaluation);
    }

    return evaluation;
}

VulkanPhysicalDeviceSelectionPlan
MakeVulkanPhysicalDeviceSelectionPlan(const std::vector<VulkanPhysicalDeviceCapabilities>& candidates,
                                      const VulkanPhysicalDeviceRequirements& requirements) {
    std::optional<std::size_t> selectedIndex;
    VulkanPhysicalDeviceEvaluation selectedEvaluation;
    std::vector<VulkanPhysicalDeviceEvaluation> evaluations;
    evaluations.reserve(candidates.size());

    for (std::size_t index = 0; index < candidates.size(); ++index) {
        VulkanPhysicalDeviceEvaluation evaluation = EvaluateVulkanPhysicalDevice(candidates[index], requirements);
        evaluations.push_back(evaluation);

        if (!evaluation.Suitable) {
            continue;
        }

        if (!selectedIndex.has_value() ||
            IsPreferredCandidate(candidates[index], evaluation, candidates[*selectedIndex], selectedEvaluation)) {
            selectedIndex = index;
            selectedEvaluation = std::move(evaluation);
        }
    }

    if (!selectedIndex.has_value()) {
        std::string diagnostic = "No suitable Vulkan physical device found";

        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const std::string name = candidates[index].Name.empty() ? "<unnamed>" : candidates[index].Name;
            diagnostic += std::format("; '{}': {}", name, JoinReasons(evaluations[index].RejectionReasons));
        }

        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "{}", diagnostic);
    }

    return {
            .CandidateIndex = *selectedIndex,
            .Score = selectedEvaluation.Score,
            .GraphicsQueueFamilyIndex = *selectedEvaluation.GraphicsQueueFamilyIndex,
            .PresentQueueFamilyIndex = *selectedEvaluation.PresentQueueFamilyIndex,
    };
}

VulkanPhysicalDeviceSelection SelectVulkanPhysicalDevice(const VulkanInstance& instance,
                                                         VkSurfaceKHR surface,
                                                         const VulkanPhysicalDeviceRequirements& requirements) {
    if (surface == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Vulkan physical device selection requires a valid surface");
    }

    const std::vector<VkPhysicalDevice> devices = EnumeratePhysicalDevices(instance.GetHandle());
    std::vector<VulkanPhysicalDeviceCapabilities> candidates;
    candidates.reserve(devices.size());

    for (VkPhysicalDevice device: devices) {
        candidates.push_back(ReadDeviceCapabilities(device, surface));
    }

    const VulkanPhysicalDeviceSelectionPlan plan = MakeVulkanPhysicalDeviceSelectionPlan(candidates, requirements);

    return {
            .Handle = devices[plan.CandidateIndex],
            .Capabilities = std::move(candidates[plan.CandidateIndex]),
            .GraphicsQueueFamilyIndex = plan.GraphicsQueueFamilyIndex,
            .PresentQueueFamilyIndex = plan.PresentQueueFamilyIndex,
    };
}

} // namespace NVulkan
