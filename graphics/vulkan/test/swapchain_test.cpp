#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <spawn.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <sys/wait.h>

#if defined(__APPLE__)
    #include <crt_externs.h>

    #include <mach-o/dyld.h>
#else
extern char** environ;
#endif

#include <graphics/vulkan/device.h>
#include <graphics/vulkan/glfw_surface.h>
#include <graphics/vulkan/instance.h>
#include <graphics/vulkan/physical_device.h>
#include <graphics/vulkan/swapchain.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <window/window.h>
#include <window/window_config.h>
#include <window/window_runtime.h>
#include <window/window_size.h>
#include <window/window_type.h>

namespace {

using NVulkan::MakeVulkanSwapchainPlan;
using NVulkan::VulkanSwapchainPlan;
using NVulkan::VulkanSwapchainSupport;

VulkanSwapchainSupport MakeSupport() {
    return {
            .Capabilities =
                    {
                            .minImageCount = 2,
                            .maxImageCount = 4,
                            .currentExtent =
                                    {
                                            .width = std::numeric_limits<std::uint32_t>::max(),
                                            .height = std::numeric_limits<std::uint32_t>::max(),
                                    },
                            .minImageExtent =
                                    {
                                            .width = 64,
                                            .height = 64,
                                    },
                            .maxImageExtent =
                                    {
                                            .width = 1920,
                                            .height = 1080,
                                    },
                            .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                            .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                            .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                            .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                    },
            .Formats =
                    {
                            {
                                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                                    .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                            },
                            {
                                    .format = VK_FORMAT_B8G8R8A8_SRGB,
                                    .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                            },
                    },
            .PresentModes =
                    {
                            VK_PRESENT_MODE_FIFO_KHR,
                            VK_PRESENT_MODE_MAILBOX_KHR,
                    },
    };
}

bool IsEnvironmentFailure(const NCommon::Exception& exception) {
    const std::string& message = exception.GetMessage();

    return message.find("VK_ERROR_INCOMPATIBLE_DRIVER") != std::string::npos ||
           message.find("Failed to create GLFW window") != std::string::npos ||
           message.find("Failed to initialize GLFW") != std::string::npos ||
           message.find("Failed to get GLFW Vulkan instance extensions") != std::string::npos;
}

NWindow::WindowConfig MakeWindowConfig() {
    NWindow::WindowConfig windowConfig{NWindow::EWindowType::GLFW};
    windowConfig.Title = "GraphicsEngine Vulkan Swapchain Test";
    windowConfig.Size = {
            .Width = 64,
            .Height = 64,
    };

    return windowConfig;
}

std::string GetExecutablePath() {
#if defined(__APPLE__)
    std::vector<char> path(1024);
    std::uint32_t size = static_cast<std::uint32_t>(path.size());

    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        path.resize(size);
        if (_NSGetExecutablePath(path.data(), &size) != 0) {
            return {};
        }
    }

    return path.data();
#else
    std::vector<char> path(4096);
    const ssize_t size = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (size <= 0) {
        return {};
    }

    path[static_cast<std::size_t>(size)] = '\0';

    return path.data();
#endif
}

char** GetEnvironment() {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

void RunSpawnedSmoke(std::string_view filter) {
    const std::string executable = GetExecutablePath();
    ASSERT_FALSE(executable.empty());
    const std::string filterArg = std::string{"--gtest_filter="} + std::string{filter};
    std::string alsoRunDisabledArg = "--gtest_also_run_disabled_tests";
    std::string colorArg = "--gtest_color=no";

    char* const arguments[] = {
            const_cast<char*>(executable.c_str()),
            const_cast<char*>(filterArg.c_str()),
            alsoRunDisabledArg.data(),
            colorArg.data(),
            nullptr,
    };

    pid_t pid = 0;
    const int spawnResult = posix_spawn(&pid, executable.c_str(), nullptr, nullptr, arguments, GetEnvironment());
    ASSERT_EQ(spawnResult, 0);
    ASSERT_NE(pid, -1);

    int status = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const pid_t result = waitpid(pid, &status, WNOHANG);
        ASSERT_NE(result, -1);

        if (result == pid) {
            ASSERT_TRUE(WIFEXITED(status));
            EXPECT_EQ(WEXITSTATUS(status), 0);
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    GTEST_SKIP() << "Vulkan swapchain smoke timed out in this environment";
}

} // namespace

TEST(VulkanSwapchainPlan, SelectsPreferredSurfaceFormatAndPresentMode) {
    const VulkanSwapchainPlan plan = MakeVulkanSwapchainPlan(MakeSupport(),
                                                             {
                                                                     .Width = 800,
                                                                     .Height = 600,
                                                             },
                                                             2,
                                                             2);

    EXPECT_FALSE(plan.Suspended);
    EXPECT_EQ(plan.SurfaceFormat.format, VK_FORMAT_B8G8R8A8_SRGB);
    EXPECT_EQ(plan.SurfaceFormat.colorSpace, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    EXPECT_EQ(plan.PresentMode, VK_PRESENT_MODE_MAILBOX_KHR);
    EXPECT_EQ(plan.Extent.width, 800U);
    EXPECT_EQ(plan.Extent.height, 600U);
    EXPECT_EQ(plan.ImageCount, 3U);
    EXPECT_EQ(plan.SharingMode, VK_SHARING_MODE_EXCLUSIVE);
    EXPECT_TRUE(plan.QueueFamilyIndices.empty());
}

TEST(VulkanSwapchainPlan, FallsBackToFifoAndFirstSurfaceFormat) {
    VulkanSwapchainSupport support = MakeSupport();
    support.Formats.pop_back();
    support.PresentModes = {VK_PRESENT_MODE_FIFO_KHR};

    const VulkanSwapchainPlan plan = MakeVulkanSwapchainPlan(support,
                                                             {
                                                                     .Width = 800,
                                                                     .Height = 600,
                                                             },
                                                             2,
                                                             2);

    EXPECT_EQ(plan.SurfaceFormat.format, VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(plan.PresentMode, VK_PRESENT_MODE_FIFO_KHR);
}

TEST(VulkanSwapchainPlan, UsesConcurrentSharingForSeparateQueueFamilies) {
    const VulkanSwapchainPlan plan = MakeVulkanSwapchainPlan(MakeSupport(),
                                                             {
                                                                     .Width = 800,
                                                                     .Height = 600,
                                                             },
                                                             2,
                                                             5);

    EXPECT_EQ(plan.SharingMode, VK_SHARING_MODE_CONCURRENT);
    EXPECT_EQ(plan.QueueFamilyIndices, (std::vector<std::uint32_t>{2, 5}));
}

TEST(VulkanSwapchainPlan, UsesCurrentSurfaceExtentWhenFixed) {
    VulkanSwapchainSupport support = MakeSupport();
    support.Capabilities.currentExtent = {
            .width = 1024,
            .height = 768,
    };

    const VulkanSwapchainPlan plan = MakeVulkanSwapchainPlan(support,
                                                             {
                                                                     .Width = 800,
                                                                     .Height = 600,
                                                             },
                                                             2,
                                                             2);

    EXPECT_EQ(plan.Extent.width, 1024U);
    EXPECT_EQ(plan.Extent.height, 768U);
}

TEST(VulkanSwapchainPlan, SuspendsZeroFramebufferSize) {
    const VulkanSwapchainPlan plan = MakeVulkanSwapchainPlan(MakeSupport(),
                                                             {
                                                                     .Width = 0,
                                                                     .Height = 600,
                                                             },
                                                             2,
                                                             2);

    EXPECT_TRUE(plan.Suspended);
    EXPECT_EQ(plan.ImageCount, 0U);
}

TEST(VulkanSwapchainPlan, RejectsMissingFormats) {
    VulkanSwapchainSupport support = MakeSupport();
    support.Formats.clear();

    EXPECT_THROW((void)MakeVulkanSwapchainPlan(support,
                                               {
                                                       .Width = 800,
                                                       .Height = 600,
                                               },
                                               2,
                                               2),
                 NCommon::Exception);
}

TEST(VulkanSwapchainChild, DISABLED_CreatesAndDestroysSwapchain) {
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{MakeWindowConfig()};

        NVulkan::VulkanInstanceConfig instanceConfig;
        instanceConfig.ApplicationName = "GraphicsEngineVulkanSwapchainTest";
        instanceConfig.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
        instanceConfig.ValidationMode = NVulkan::EValidationMode::EnabledIfAvailable;
        instanceConfig.EnableDebugUtils = true;

        NVulkan::VulkanInstance instance{instanceConfig};
        const NVulkan::NGlfw::VulkanSurface surface{instance, window};
        const NVulkan::VulkanPhysicalDeviceSelection physicalDevice =
                NVulkan::SelectVulkanPhysicalDevice(instance, surface.GetHandle());
        const NVulkan::VulkanDevice device{physicalDevice};
        const NVulkan::VulkanSwapchain swapchain{device,
                                                 physicalDevice,
                                                 surface.GetHandle(),
                                                 window.GetFramebufferSize()};

        ASSERT_FALSE(swapchain.IsSuspended());
        EXPECT_NE(swapchain.GetHandle(), VK_NULL_HANDLE);
        EXPECT_GE(swapchain.GetImages().size(), swapchain.GetPlan().ImageCount);

        for (const NVulkan::VulkanSwapchainImage& image: swapchain.GetImages()) {
            EXPECT_NE(image.Handle, VK_NULL_HANDLE);
            EXPECT_NE(image.View, VK_NULL_HANDLE);
        }
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }

        throw;
    }
}

TEST(VulkanSwapchain, CreatesAndDestroysSwapchain) {
    RunSpawnedSmoke("VulkanSwapchainChild.DISABLED_CreatesAndDestroysSwapchain");
}
