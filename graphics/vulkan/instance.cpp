#include "instance.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

namespace {

constexpr std::string_view ValidationLayer = "VK_LAYER_KHRONOS_validation";

std::string_view GetVkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

bool Contains(const std::vector<std::string>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

void AppendUnique(std::vector<std::string>& values, std::string_view value) {
    if (Contains(values, value)) {
        return;
    }

    values.emplace_back(value);
}

void RequireExtension(const VulkanInstanceEnvironment& environment, std::string_view extension) {
    if (Contains(environment.Extensions, extension)) {
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED,
                          "Required Vulkan instance extension '{}' is not available",
                          extension);
}

std::vector<const char*> MakeNamePointers(const std::vector<std::string>& values) {
    std::vector<const char*> names;
    names.reserve(values.size());

    for (const std::string& value: values) {
        names.push_back(value.c_str());
    }

    return names;
}

std::vector<std::string> EnumerateInstanceExtensions() {
    std::uint32_t count = 0;

    VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to enumerate Vulkan instance extensions: {}",
                              GetVkResultName(result));
    }

    std::vector<VkExtensionProperties> properties(count);

    result = vkEnumerateInstanceExtensionProperties(nullptr, &count, properties.data());
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to read Vulkan instance extensions: {}",
                              GetVkResultName(result));
    }

    std::vector<std::string> extensions;
    extensions.reserve(count);

    for (const VkExtensionProperties& property: properties) {
        extensions.emplace_back(property.extensionName);
    }

    return extensions;
}

std::vector<std::string> EnumerateInstanceLayers() {
    std::uint32_t count = 0;

    VkResult result = vkEnumerateInstanceLayerProperties(&count, nullptr);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to enumerate Vulkan instance layers: {}",
                              GetVkResultName(result));
    }

    std::vector<VkLayerProperties> properties(count);

    result = vkEnumerateInstanceLayerProperties(&count, properties.data());
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to read Vulkan instance layers: {}",
                              GetVkResultName(result));
    }

    std::vector<std::string> layers;
    layers.reserve(count);

    for (const VkLayerProperties& property: properties) {
        layers.emplace_back(property.layerName);
    }

    return layers;
}

std::uint32_t EnumerateInstanceApiVersion() {
    std::uint32_t version = VK_API_VERSION_1_0;

    if (vkEnumerateInstanceVersion == nullptr) {
        return version;
    }

    const VkResult result = vkEnumerateInstanceVersion(&version);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to enumerate Vulkan instance API version: {}",
                              GetVkResultName(result));
    }

    return version;
}

VulkanInstanceEnvironment ReadEnvironment() {
    const VkResult result = volkInitialize();
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to initialize Volk Vulkan loader: {}",
                              GetVkResultName(result));
    }

    return {
            .ApiVersion = EnumerateInstanceApiVersion(),
            .Extensions = EnumerateInstanceExtensions(),
            .Layers = EnumerateInstanceLayers(),
    };
}

VKAPI_ATTR VkBool32 VKAPI_CALL HandleDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT type,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                  void* userData) {
    auto* handler = static_cast<VulkanDebugMessageHandler*>(userData);

    if (handler != nullptr && *handler) {
        try {
            (*handler)({
                    .Severity = severity,
                    .Type = type,
                    .Message = data != nullptr && data->pMessage != nullptr ? data->pMessage : "",
            });
        } catch (...) {
        }
    }

    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT MakeDebugMessengerCreateInfo(VulkanDebugMessageHandler* handler) {
    return {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,
            .messageSeverity =
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = &HandleDebugMessage,
            .pUserData = handler,
    };
}

VkInstanceCreateFlags MakeInstanceFlags(const VulkanInstancePlan& plan) {
    VkInstanceCreateFlags flags = 0;

    if (plan.PortabilityEnumerationEnabled) {
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    return flags;
}

} // namespace

VulkanInstancePlan MakeVulkanInstancePlan(const VulkanInstanceConfig& config,
                                          const VulkanInstanceEnvironment& environment) {
    if (environment.ApiVersion < config.RequiredApiVersion) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED,
                              "Vulkan instance API version {}.{}.{} is below required {}.{}.{}",
                              VK_API_VERSION_MAJOR(environment.ApiVersion),
                              VK_API_VERSION_MINOR(environment.ApiVersion),
                              VK_API_VERSION_PATCH(environment.ApiVersion),
                              VK_API_VERSION_MAJOR(config.RequiredApiVersion),
                              VK_API_VERSION_MINOR(config.RequiredApiVersion),
                              VK_API_VERSION_PATCH(config.RequiredApiVersion));
    }

    VulkanInstancePlan plan{
            .ApiVersion = config.RequiredApiVersion,
    };

    for (const std::string& extension: config.RequiredExtensions) {
        RequireExtension(environment, extension);
        AppendUnique(plan.Extensions, extension);
    }

    for (const std::string& extension: config.OptionalExtensions) {
        if (Contains(environment.Extensions, extension)) {
            AppendUnique(plan.Extensions, extension);
        }
    }

    if (config.EnablePortabilityEnumeration) {
        RequireExtension(environment, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        AppendUnique(plan.Extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        plan.PortabilityEnumerationEnabled = true;
    }

    if (config.ValidationMode != EValidationMode::Disabled) {
        const bool validationAvailable = Contains(environment.Layers, ValidationLayer);
        if (!validationAvailable && config.ValidationMode == EValidationMode::Required) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED,
                                  "Required Vulkan validation layer '{}' is not available",
                                  ValidationLayer);
        }

        if (validationAvailable) {
            AppendUnique(plan.Layers, ValidationLayer);
            plan.ValidationEnabled = true;
        }
    }

    if (config.EnableDebugUtils && Contains(environment.Extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        AppendUnique(plan.Extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        plan.DebugUtilsEnabled = true;
    }

    return plan;
}

class VulkanInstance::Impl final: public NCommon::NonTransferable {
public:
    explicit Impl(const VulkanInstanceConfig& config)
        : m_plan(MakeVulkanInstancePlan(config, ReadEnvironment()))
        , m_debugMessageHandler(config.DebugMessageHandler)
        , m_instance(CreateInstance(config)) {
        try {
            volkLoadInstanceOnly(m_instance);
            CreateDebugMessenger();
        } catch (...) {
            Destroy();
            throw;
        }
    }

    ~Impl() {
        Destroy();
    }

    void Destroy() noexcept {
        if (m_debugMessenger != VK_NULL_HANDLE) {
            vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
            m_debugMessenger = VK_NULL_HANDLE;
        }

        if (m_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] VkInstance GetHandle() const noexcept {
        return m_instance;
    }

    [[nodiscard]] const VulkanInstancePlan& GetPlan() const noexcept {
        return m_plan;
    }

private:
    VkInstance CreateInstance(const VulkanInstanceConfig& config) {
        const std::vector<const char*> extensionNames = MakeNamePointers(m_plan.Extensions);
        const std::vector<const char*> layerNames = MakeNamePointers(m_plan.Layers);

        const VkApplicationInfo applicationInfo{
                .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                .pNext = nullptr,
                .pApplicationName = config.ApplicationName.c_str(),
                .applicationVersion = config.ApplicationVersion,
                .pEngineName = "GraphicsEngine",
                .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
                .apiVersion = m_plan.ApiVersion,
        };

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        const void* next = nullptr;
        if (m_plan.DebugUtilsEnabled) {
            debugCreateInfo = MakeDebugMessengerCreateInfo(&m_debugMessageHandler);
            next = &debugCreateInfo;
        }

        const VkInstanceCreateInfo createInfo{
                .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                .pNext = next,
                .flags = MakeInstanceFlags(m_plan),
                .pApplicationInfo = &applicationInfo,
                .enabledLayerCount = static_cast<std::uint32_t>(layerNames.size()),
                .ppEnabledLayerNames = layerNames.data(),
                .enabledExtensionCount = static_cast<std::uint32_t>(extensionNames.size()),
                .ppEnabledExtensionNames = extensionNames.data(),
        };

        VkInstance instance = VK_NULL_HANDLE;
        const VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to create Vulkan instance: {}",
                                  GetVkResultName(result));
        }

        return instance;
    }

    void CreateDebugMessenger() {
        if (!m_plan.DebugUtilsEnabled) {
            return;
        }

        VkDebugUtilsMessengerCreateInfoEXT createInfo = MakeDebugMessengerCreateInfo(&m_debugMessageHandler);
        const VkResult result = vkCreateDebugUtilsMessengerEXT(m_instance, &createInfo, nullptr, &m_debugMessenger);
        if (result != VK_SUCCESS) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Failed to create Vulkan debug utils messenger: {}",
                                  GetVkResultName(result));
        }
    }

private:
    VulkanInstancePlan m_plan;
    VulkanDebugMessageHandler m_debugMessageHandler;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
};

VulkanInstance::VulkanInstance(const VulkanInstanceConfig& config)
    : m_impl(std::make_unique<Impl>(config)) {
}

VulkanInstance::~VulkanInstance() = default;

VkInstance VulkanInstance::GetHandle() const noexcept {
    return m_impl->GetHandle();
}

std::uint32_t VulkanInstance::GetApiVersion() const noexcept {
    return m_impl->GetPlan().ApiVersion;
}

bool VulkanInstance::IsDebugUtilsEnabled() const noexcept {
    return m_impl->GetPlan().DebugUtilsEnabled;
}

bool VulkanInstance::IsValidationEnabled() const noexcept {
    return m_impl->GetPlan().ValidationEnabled;
}

const std::vector<std::string>& VulkanInstance::GetEnabledExtensions() const noexcept {
    return m_impl->GetPlan().Extensions;
}

} // namespace NVulkan
