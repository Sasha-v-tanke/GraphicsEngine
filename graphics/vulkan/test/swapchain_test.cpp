#include <cstdint>
#include <limits>
#include <vector>

#include <graphics/vulkan/swapchain.h>
#include <gtest/gtest.h>

namespace {

constexpr std::uint32_t DynamicExtent = std::numeric_limits<std::uint32_t>::max();

NVulkan::VulkanSwapchainSupport MakeSupport() {
    return {
            .Capabilities =
                    {
                            .minImageCount = 2,
                            .maxImageCount = 4,
                            .currentExtent = {.width = DynamicExtent, .height = DynamicExtent},
                            .minImageExtent = {.width = 32, .height = 16},
                            .maxImageExtent = {.width = 1920, .height = 1080},
                            .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                    },
            .Formats =
                    {
                            {.format = VK_FORMAT_R8G8B8A8_UNORM, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                            {.format = VK_FORMAT_B8G8R8A8_SRGB, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                    },
            .PresentModes =
                    {
                            VK_PRESENT_MODE_FIFO_KHR,
                            VK_PRESENT_MODE_MAILBOX_KHR,
                    },
    };
}

VkExtent2D MakeExtent(std::uint32_t width, std::uint32_t height) noexcept {
    return {
            .width = width,
            .height = height,
    };
}

} // namespace

TEST(VulkanSwapchainConfig, ChoosesPreferredFormatPresentModeAndClampedExtent) {
    const NVulkan::VulkanSwapchainConfig config =
            NVulkan::MakeVulkanSwapchainConfig(MakeSupport(), MakeExtent(4096, 8), 2, 2);

    EXPECT_EQ(config.ImageFormat, VK_FORMAT_B8G8R8A8_SRGB);
    EXPECT_EQ(config.ColorSpace, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    EXPECT_EQ(config.PresentMode, VK_PRESENT_MODE_MAILBOX_KHR);
    EXPECT_EQ(config.Extent.width, 1920U);
    EXPECT_EQ(config.Extent.height, 16U);
    EXPECT_EQ(config.ImageCount, 3U);
    EXPECT_EQ(config.SharingMode, VK_SHARING_MODE_EXCLUSIVE);
    EXPECT_TRUE(config.QueueFamilyIndices.empty());
    EXPECT_FALSE(config.Suspended);
}

TEST(VulkanSwapchainConfig, UsesConcurrentSharingForSeparateGraphicsAndPresentFamilies) {
    const NVulkan::VulkanSwapchainConfig config =
            NVulkan::MakeVulkanSwapchainConfig(MakeSupport(), MakeExtent(64, 64), 2, 5);

    EXPECT_EQ(config.SharingMode, VK_SHARING_MODE_CONCURRENT);
    EXPECT_EQ(config.QueueFamilyIndices, (std::vector<std::uint32_t>{2, 5}));
}

TEST(VulkanSwapchainConfig, UsesCurrentExtentWhenSurfaceIsFixed) {
    NVulkan::VulkanSwapchainSupport support = MakeSupport();
    support.Capabilities.currentExtent = {.width = 800, .height = 600};

    const NVulkan::VulkanSwapchainConfig config = NVulkan::MakeVulkanSwapchainConfig(support, MakeExtent(64, 64), 2, 2);

    EXPECT_EQ(config.Extent.width, 800U);
    EXPECT_EQ(config.Extent.height, 600U);
}

TEST(VulkanSwapchainConfig, SuspendsZeroSizeFramebuffer) {
    const NVulkan::VulkanSwapchainConfig config =
            NVulkan::MakeVulkanSwapchainConfig(MakeSupport(), MakeExtent(0, 64), 2, 2);

    EXPECT_TRUE(config.Suspended);
}

TEST(VulkanSwapchainConfig, SuspendsZeroSizeFramebufferWithFixedSurfaceExtent) {
    NVulkan::VulkanSwapchainSupport support = MakeSupport();
    support.Capabilities.currentExtent = {.width = 800, .height = 600};

    const NVulkan::VulkanSwapchainConfig config = NVulkan::MakeVulkanSwapchainConfig(support, MakeExtent(0, 64), 2, 2);

    EXPECT_TRUE(config.Suspended);
    EXPECT_EQ(config.Extent.width, 0U);
    EXPECT_EQ(config.Extent.height, 64U);
}

TEST(VulkanSwapchainConfig, FallsBackToFifoPresentMode) {
    NVulkan::VulkanSwapchainSupport support = MakeSupport();
    support.PresentModes = {VK_PRESENT_MODE_FIFO_KHR};

    const NVulkan::VulkanSwapchainConfig config = NVulkan::MakeVulkanSwapchainConfig(support, MakeExtent(64, 64), 2, 2);

    EXPECT_EQ(config.PresentMode, VK_PRESENT_MODE_FIFO_KHR);
}

TEST(VulkanSwapchainConfig, SelectsSupportedCompositeAlpha) {
    NVulkan::VulkanSwapchainSupport support = MakeSupport();
    support.Capabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;

    const NVulkan::VulkanSwapchainConfig config = NVulkan::MakeVulkanSwapchainConfig(support, MakeExtent(64, 64), 2, 2);

    EXPECT_EQ(config.CompositeAlpha, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR);
}
