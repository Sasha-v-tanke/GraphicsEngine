#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <graphics/vulkan/instance.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>

namespace {

bool Contains(const std::vector<std::string>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

NVulkan::VulkanInstanceEnvironment MakeEnvironment() {
    return {
            .ApiVersion = VK_API_VERSION_1_3,
            .Extensions =
                    {
                            VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
                            "VK_TEST_required",
                            "VK_TEST_optional",
                    },
            .Layers =
                    {
                            "VK_LAYER_KHRONOS_validation",
                    },
    };
}

TEST(VulkanInstancePlan, EnablesRequiredOptionalDebugValidationAndPortability) {
    NVulkan::VulkanInstanceConfig config;
    config.RequiredExtensions = {
            "VK_TEST_required",
    };
    config.OptionalExtensions = {
            "VK_TEST_optional",
            "VK_TEST_missing_optional",
    };
    config.ValidationMode = NVulkan::EValidationMode::Required;
    config.EnableDebugUtils = true;
    config.EnablePortabilityEnumeration = true;

    const NVulkan::VulkanInstancePlan plan = NVulkan::MakeVulkanInstancePlan(config, MakeEnvironment());

    EXPECT_EQ(plan.ApiVersion, VK_API_VERSION_1_3);
    EXPECT_TRUE(plan.ValidationEnabled);
    EXPECT_TRUE(plan.DebugUtilsEnabled);
    EXPECT_TRUE(plan.PortabilityEnumerationEnabled);
    EXPECT_TRUE(Contains(plan.Layers, "VK_LAYER_KHRONOS_validation"));
    EXPECT_TRUE(Contains(plan.Extensions, "VK_TEST_required"));
    EXPECT_TRUE(Contains(plan.Extensions, "VK_TEST_optional"));
    EXPECT_TRUE(Contains(plan.Extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
    EXPECT_TRUE(Contains(plan.Extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME));
    EXPECT_FALSE(Contains(plan.Extensions, "VK_TEST_missing_optional"));
}

TEST(VulkanInstancePlan, ThrowsWhenApiVersionIsTooOld) {
    NVulkan::VulkanInstanceConfig config;
    config.RequiredApiVersion = VK_API_VERSION_1_3;

    NVulkan::VulkanInstanceEnvironment environment = MakeEnvironment();
    environment.ApiVersion = VK_API_VERSION_1_2;

    EXPECT_THROW((void)NVulkan::MakeVulkanInstancePlan(config, environment), NCommon::Exception);
}

TEST(VulkanInstancePlan, ThrowsWhenRequiredExtensionIsMissing) {
    NVulkan::VulkanInstanceConfig config;
    config.RequiredExtensions = {
            "VK_TEST_missing_required",
    };
    config.EnablePortabilityEnumeration = false;

    EXPECT_THROW((void)NVulkan::MakeVulkanInstancePlan(config, MakeEnvironment()), NCommon::Exception);
}

TEST(VulkanInstancePlan, ThrowsWhenRequiredValidationLayerIsMissing) {
    NVulkan::VulkanInstanceConfig config;
    config.ValidationMode = NVulkan::EValidationMode::Required;
    config.EnablePortabilityEnumeration = false;

    NVulkan::VulkanInstanceEnvironment environment = MakeEnvironment();
    environment.Layers.clear();

    EXPECT_THROW((void)NVulkan::MakeVulkanInstancePlan(config, environment), NCommon::Exception);
}

TEST(VulkanInstancePlan, SkipsOptionalValidationLayerWhenMissing) {
    NVulkan::VulkanInstanceConfig config;
    config.ValidationMode = NVulkan::EValidationMode::EnabledIfAvailable;
    config.EnablePortabilityEnumeration = false;

    NVulkan::VulkanInstanceEnvironment environment = MakeEnvironment();
    environment.Layers.clear();

    const NVulkan::VulkanInstancePlan plan = NVulkan::MakeVulkanInstancePlan(config, environment);

    EXPECT_FALSE(plan.ValidationEnabled);
    EXPECT_TRUE(plan.Layers.empty());
}

TEST(VulkanInstance, CreatesAndDestroysInstance) {
    NVulkan::VulkanInstanceConfig config;
    config.ApplicationName = "GraphicsEngineVulkanInstanceTest";
    config.ValidationMode = NVulkan::EValidationMode::EnabledIfAvailable;

    try {
        const NVulkan::VulkanInstance instance{config};

        EXPECT_NE(instance.GetHandle(), VK_NULL_HANDLE);
        EXPECT_EQ(instance.GetApiVersion(), VK_API_VERSION_1_3);
    } catch (const NCommon::Exception& exception) {
        if (exception.GetMessage().find("VK_ERROR_INCOMPATIBLE_DRIVER") != std::string::npos) {
            GTEST_SKIP() << exception.GetMessage();
        }

        throw;
    }
}

} // namespace
