#include <cstdint>
#include <string>
#include <vector>

#include <graphics/vulkan/device.h>
#include <graphics/vulkan/physical_device.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>

namespace {

constexpr std::string_view PortabilitySubsetExtension = "VK_KHR_portability_subset";

NVulkan::VulkanPhysicalDeviceSelection MakeSelection() {
    NVulkan::VulkanPhysicalDeviceSelection selection{
            .Handle = reinterpret_cast<VkPhysicalDevice>(1),
            .Capabilities =
                    {
                            .ApiVersion = VK_API_VERSION_1_3,
                            .Extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME},
                            .Features =
                                    {
                                            .TimelineSemaphore = true,
                                            .Synchronization2 = true,
                                            .DynamicRendering = true,
                                    },
                            .SurfaceSupported = true,
                            .SwapchainFormatsSupported = true,
                            .SwapchainPresentModesSupported = true,
                    },
            .GraphicsQueueFamilyIndex = 2,
            .PresentQueueFamilyIndex = 2,
    };

    return selection;
}

TEST(VulkanDevicePlan, UsesOneQueueCreateInfoForSharedGraphicsPresentFamily) {
    const NVulkan::VulkanDevicePlan plan = NVulkan::MakeVulkanDevicePlan(MakeSelection());

    ASSERT_EQ(plan.QueueFamilies.size(), 1U);
    EXPECT_EQ(plan.QueueFamilies.front().FamilyIndex, 2U);
    EXPECT_EQ(plan.Extensions, std::vector<std::string>{VK_KHR_SWAPCHAIN_EXTENSION_NAME});
}

TEST(VulkanDevicePlan, UsesSeparateQueueFamiliesWhenGraphicsAndPresentDiffer) {
    NVulkan::VulkanPhysicalDeviceSelection selection = MakeSelection();
    selection.PresentQueueFamilyIndex = 5;

    const NVulkan::VulkanDevicePlan plan = NVulkan::MakeVulkanDevicePlan(selection);

    ASSERT_EQ(plan.QueueFamilies.size(), 2U);
    EXPECT_EQ(plan.QueueFamilies[0].FamilyIndex, 2U);
    EXPECT_EQ(plan.QueueFamilies[1].FamilyIndex, 5U);
}

TEST(VulkanDevicePlan, EnablesPortabilitySubsetWhenAdvertised) {
    NVulkan::VulkanPhysicalDeviceSelection selection = MakeSelection();
    selection.Capabilities.Extensions.emplace_back(PortabilitySubsetExtension);

    const NVulkan::VulkanDevicePlan plan = NVulkan::MakeVulkanDevicePlan(selection);

    EXPECT_EQ(plan.Extensions,
              (std::vector<std::string>{
                      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                      std::string{PortabilitySubsetExtension},
              }));
}

TEST(VulkanDevicePlan, MapsGraphicsCapabilities) {
    NVulkan::VulkanPhysicalDeviceSelection selection = MakeSelection();

    const NVulkan::VulkanDevicePlan plan = NVulkan::MakeVulkanDevicePlan(selection);

    EXPECT_TRUE(plan.GraphicsCapabilities.Presentation);
    EXPECT_TRUE(plan.GraphicsCapabilities.TimelineCompletion);
    EXPECT_EQ(plan.GraphicsCapabilities.MaxFramesInFlight, 2U);
}

TEST(VulkanDevicePlan, RejectsNullPhysicalDevice) {
    NVulkan::VulkanPhysicalDeviceSelection selection = MakeSelection();
    selection.Handle = VK_NULL_HANDLE;

    EXPECT_THROW((void)NVulkan::MakeVulkanDevicePlan(selection), NCommon::Exception);
}

} // namespace
