#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <vulkan.h>

#include <lib/common/wrapper/non_transferable.h>

namespace NVulkan {

enum class EValidationMode {
    Disabled,
    EnabledIfAvailable,
    Required,
};

struct VulkanDebugMessage {
    VkDebugUtilsMessageSeverityFlagBitsEXT Severity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    VkDebugUtilsMessageTypeFlagsEXT Type = 0;
    std::string Message;
};

using VulkanDebugMessageHandler = std::function<void(const VulkanDebugMessage&)>;

struct VulkanInstanceConfig {
    std::string ApplicationName = "GraphicsEngine";
    std::uint32_t ApplicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    std::uint32_t RequiredApiVersion = VK_API_VERSION_1_3;
    std::vector<std::string> RequiredExtensions;
    std::vector<std::string> OptionalExtensions;
    EValidationMode ValidationMode =
#if defined(NDEBUG)
            EValidationMode::Disabled;
#else
            EValidationMode::EnabledIfAvailable;
#endif
    bool EnableDebugUtils =
#if defined(NDEBUG)
            false;
#else
            true;
#endif
    bool EnablePortabilityEnumeration =
#if defined(__APPLE__)
            true;
#else
            false;
#endif
    VulkanDebugMessageHandler DebugMessageHandler;
};

struct VulkanInstanceEnvironment {
    std::uint32_t ApiVersion = VK_API_VERSION_1_0;
    std::vector<std::string> Extensions;
    std::vector<std::string> Layers;
};

struct VulkanInstancePlan {
    std::uint32_t ApiVersion = VK_API_VERSION_1_0;
    std::vector<std::string> Extensions;
    std::vector<std::string> Layers;
    bool DebugUtilsEnabled = false;
    bool ValidationEnabled = false;
    bool PortabilityEnumerationEnabled = false;
};

VulkanInstancePlan MakeVulkanInstancePlan(const VulkanInstanceConfig& config,
                                          const VulkanInstanceEnvironment& environment);

class VulkanInstance final: public NCommon::NonTransferable {
public:
    explicit VulkanInstance(const VulkanInstanceConfig& config = {});

    ~VulkanInstance();

    [[nodiscard]] VkInstance GetHandle() const noexcept;

    [[nodiscard]] std::uint32_t GetApiVersion() const noexcept;

    [[nodiscard]] bool IsDebugUtilsEnabled() const noexcept;

    [[nodiscard]] bool IsValidationEnabled() const noexcept;

    [[nodiscard]] const std::vector<std::string>& GetEnabledExtensions() const noexcept;

private:
    class Impl;

    std::unique_ptr<Impl> m_impl;
};

} // namespace NVulkan
