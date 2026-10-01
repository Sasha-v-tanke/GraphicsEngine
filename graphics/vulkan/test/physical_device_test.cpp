#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <graphics/vulkan/physical_device.h>
#include <gtest/gtest.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace {

NVulkan::VulkanPhysicalDeviceCapabilities
MakeSuitableDevice(std::string name, NVulkan::EVulkanPhysicalDeviceType type, std::uint8_t uuidByte) {
    NVulkan::VulkanPhysicalDeviceCapabilities capabilities{
            .Name = std::move(name),
            .ApiVersion = VK_API_VERSION_1_3,
            .VendorId = 1,
            .DeviceId = 1,
            .Type = type,
            .MaxImageDimension2D = 8192,
            .Extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME},
            .Features =
                    {
                            .TimelineSemaphore = true,
                            .Synchronization2 = true,
                            .DynamicRendering = true,
                    },
            .QueueFamilies =
                    {
                            {
                                    .Index = 0,
                                    .QueueCount = 1,
                                    .Graphics = true,
                                    .Present = true,
                            },
                    },
            .SurfaceSupported = true,
            .SwapchainFormatsSupported = true,
            .SwapchainPresentModesSupported = true,
    };
    capabilities.DeviceUuid[0] = uuidByte;

    return capabilities;
}

bool HasReason(const NVulkan::VulkanPhysicalDeviceEvaluation& evaluation, std::string_view value) {
    return std::ranges::any_of(evaluation.RejectionReasons,
                               [value](const std::string& reason) { return reason.find(value) != std::string::npos; });
}

TEST(VulkanPhysicalDeviceEvaluation, ReportsAllMissingRequirements) {
    NVulkan::VulkanPhysicalDeviceCapabilities capabilities;
    capabilities.Name = "Unsupported GPU";
    capabilities.ApiVersion = VK_API_VERSION_1_2;

    const NVulkan::VulkanPhysicalDeviceEvaluation evaluation = NVulkan::EvaluateVulkanPhysicalDevice(capabilities);

    EXPECT_FALSE(evaluation.Suitable);
    EXPECT_EQ(evaluation.Score, 0U);
    EXPECT_TRUE(HasReason(evaluation, "API version"));
    EXPECT_TRUE(HasReason(evaluation, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
    EXPECT_TRUE(HasReason(evaluation, "timelineSemaphore"));
    EXPECT_TRUE(HasReason(evaluation, "synchronization2"));
    EXPECT_TRUE(HasReason(evaluation, "dynamicRendering"));
    EXPECT_TRUE(HasReason(evaluation, "graphics queue"));
    EXPECT_TRUE(HasReason(evaluation, "present queue"));
    EXPECT_TRUE(HasReason(evaluation, "surface"));
    EXPECT_TRUE(HasReason(evaluation, "swapchain formats"));
    EXPECT_TRUE(HasReason(evaluation, "swapchain present modes"));
}

TEST(VulkanPhysicalDeviceEvaluation, PrefersSharedGraphicsPresentQueue) {
    NVulkan::VulkanPhysicalDeviceCapabilities capabilities =
            MakeSuitableDevice("Shared Queue GPU", NVulkan::EVulkanPhysicalDeviceType::INTEGRATED_GPU, 1);
    capabilities.QueueFamilies = {
            {
                    .Index = 1,
                    .QueueCount = 1,
                    .Graphics = true,
            },
            {
                    .Index = 2,
                    .QueueCount = 1,
                    .Present = true,
            },
            {
                    .Index = 3,
                    .QueueCount = 1,
                    .Graphics = true,
                    .Present = true,
            },
    };

    const NVulkan::VulkanPhysicalDeviceEvaluation evaluation = NVulkan::EvaluateVulkanPhysicalDevice(capabilities);

    ASSERT_TRUE(evaluation.Suitable);
    ASSERT_TRUE(evaluation.GraphicsQueueFamilyIndex.has_value());
    ASSERT_TRUE(evaluation.PresentQueueFamilyIndex.has_value());
    EXPECT_EQ(*evaluation.GraphicsQueueFamilyIndex, 3U);
    EXPECT_EQ(*evaluation.PresentQueueFamilyIndex, 3U);
}

TEST(VulkanPhysicalDeviceSelection, SelectsHighestScoringSuitableDevice) {
    NVulkan::VulkanPhysicalDeviceCapabilities integrated =
            MakeSuitableDevice("Integrated", NVulkan::EVulkanPhysicalDeviceType::INTEGRATED_GPU, 1);
    integrated.MaxImageDimension2D = 16384;

    NVulkan::VulkanPhysicalDeviceCapabilities discrete =
            MakeSuitableDevice("Discrete", NVulkan::EVulkanPhysicalDeviceType::DISCRETE_GPU, 2);
    discrete.MaxImageDimension2D = 4096;

    const std::vector<NVulkan::VulkanPhysicalDeviceCapabilities> candidates = {integrated, discrete};
    const NVulkan::VulkanPhysicalDeviceSelectionPlan plan = NVulkan::MakeVulkanPhysicalDeviceSelectionPlan(candidates);

    EXPECT_EQ(plan.CandidateIndex, 1U);
    EXPECT_GT(plan.Score, 0U);
}

TEST(VulkanPhysicalDeviceSelection, ThrowsWhenNoSuitableGpuExists) {
    NVulkan::VulkanPhysicalDeviceCapabilities first =
            MakeSuitableDevice("Old GPU", NVulkan::EVulkanPhysicalDeviceType::DISCRETE_GPU, 1);
    first.ApiVersion = VK_API_VERSION_1_2;

    NVulkan::VulkanPhysicalDeviceCapabilities second =
            MakeSuitableDevice("No Present GPU", NVulkan::EVulkanPhysicalDeviceType::INTEGRATED_GPU, 2);
    second.QueueFamilies.front().Present = false;

    try {
        static_cast<void>(NVulkan::MakeVulkanPhysicalDeviceSelectionPlan({first, second}));
        FAIL() << "Expected NCommon::Exception";
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(exception.code(), NCommon::make_error_code(NCommon::EError::UNSUPPORTED));
        EXPECT_NE(exception.GetMessage().find("Old GPU"), std::string::npos);
        EXPECT_NE(exception.GetMessage().find("No Present GPU"), std::string::npos);
        EXPECT_NE(exception.GetMessage().find("API version"), std::string::npos);
        EXPECT_NE(exception.GetMessage().find("present queue"), std::string::npos);
    }
}

TEST(VulkanPhysicalDeviceSelection, IsDeterministicAcrossCandidateOrder) {
    NVulkan::VulkanPhysicalDeviceCapabilities uuidTwo =
            MakeSuitableDevice("Same GPU", NVulkan::EVulkanPhysicalDeviceType::DISCRETE_GPU, 2);
    NVulkan::VulkanPhysicalDeviceCapabilities uuidOne =
            MakeSuitableDevice("Same GPU", NVulkan::EVulkanPhysicalDeviceType::DISCRETE_GPU, 1);

    const std::vector<NVulkan::VulkanPhysicalDeviceCapabilities> forward = {uuidTwo, uuidOne};
    const std::vector<NVulkan::VulkanPhysicalDeviceCapabilities> reverse = {uuidOne, uuidTwo};

    const NVulkan::VulkanPhysicalDeviceSelectionPlan forwardPlan =
            NVulkan::MakeVulkanPhysicalDeviceSelectionPlan(forward);
    const NVulkan::VulkanPhysicalDeviceSelectionPlan reversePlan =
            NVulkan::MakeVulkanPhysicalDeviceSelectionPlan(reverse);

    EXPECT_EQ(forward[forwardPlan.CandidateIndex].DeviceUuid[0], 1U);
    EXPECT_EQ(reverse[reversePlan.CandidateIndex].DeviceUuid[0], 1U);
    EXPECT_EQ(forwardPlan.Score, reversePlan.Score);
}

} // namespace
