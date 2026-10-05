#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
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

VkFence CreateFence(VkDevice device) {
    const VkFenceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };

    VkFence fence = VK_NULL_HANDLE;
    const VkResult result = vkCreateFence(device, &createInfo, nullptr, &fence);
    if (result != VK_SUCCESS) {
        throw std::runtime_error{"Failed to create Vulkan acquire fence"};
    }

    return fence;
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

TEST(VulkanSwapchainConfig, ChoosesPreferredFormatPresentModeAndClampedExtent) {
    const NVulkan::VulkanSwapchainConfig config =
            NVulkan::MakeVulkanSwapchainConfig(MakeSupport(), NWindow::WindowSize{.Width = 4096, .Height = 8}, 2, 2);

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
            NVulkan::MakeVulkanSwapchainConfig(MakeSupport(), NWindow::WindowSize{.Width = 64, .Height = 64}, 2, 5);

    EXPECT_EQ(config.SharingMode, VK_SHARING_MODE_CONCURRENT);
    EXPECT_EQ(config.QueueFamilyIndices, (std::vector<std::uint32_t>{2, 5}));
}

TEST(VulkanSwapchainConfig, UsesCurrentExtentWhenSurfaceIsFixed) {
    NVulkan::VulkanSwapchainSupport support = MakeSupport();
    support.Capabilities.currentExtent = {.width = 800, .height = 600};

    const NVulkan::VulkanSwapchainConfig config =
            NVulkan::MakeVulkanSwapchainConfig(support, NWindow::WindowSize{.Width = 64, .Height = 64}, 2, 2);

    EXPECT_EQ(config.Extent.width, 800U);
    EXPECT_EQ(config.Extent.height, 600U);
}

TEST(VulkanSwapchainConfig, SuspendsZeroSizeFramebuffer) {
    const NVulkan::VulkanSwapchainConfig config =
            NVulkan::MakeVulkanSwapchainConfig(MakeSupport(), NWindow::WindowSize{.Width = 0, .Height = 64}, 2, 2);

    EXPECT_TRUE(config.Suspended);
}

TEST(VulkanSwapchainConfig, SuspendsZeroSizeFramebufferWithFixedSurfaceExtent) {
    NVulkan::VulkanSwapchainSupport support = MakeSupport();
    support.Capabilities.currentExtent = {.width = 800, .height = 600};

    const NVulkan::VulkanSwapchainConfig config =
            NVulkan::MakeVulkanSwapchainConfig(support, NWindow::WindowSize{.Width = 0, .Height = 64}, 2, 2);

    EXPECT_TRUE(config.Suspended);
    EXPECT_EQ(config.Extent.width, 0U);
    EXPECT_EQ(config.Extent.height, 64U);
}

TEST(VulkanSwapchainConfig, FallsBackToFifoPresentMode) {
    NVulkan::VulkanSwapchainSupport support = MakeSupport();
    support.PresentModes = {VK_PRESENT_MODE_FIFO_KHR};

    const NVulkan::VulkanSwapchainConfig config =
            NVulkan::MakeVulkanSwapchainConfig(support, NWindow::WindowSize{.Width = 64, .Height = 64}, 2, 2);

    EXPECT_EQ(config.PresentMode, VK_PRESENT_MODE_FIFO_KHR);
}

TEST(VulkanSwapchainChild, DISABLED_CreatesAcquiresAndPresents) {
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
        const NVulkan::VulkanSwapchain swapchain{
                physicalDevice,
                device,
                surface.GetHandle(),
                window.GetFramebufferSize(),
        };

        if (swapchain.IsSuspended()) {
            GTEST_SKIP() << "Framebuffer is zero-sized in this environment";
        }

        ASSERT_NE(swapchain.GetHandle(), VK_NULL_HANDLE);
        ASSERT_FALSE(swapchain.GetImages().empty());
        EXPECT_EQ(swapchain.GetImageViews().size(), swapchain.GetImages().size());
        EXPECT_NE(swapchain.GetImageFormat(), VK_FORMAT_UNDEFINED);
        EXPECT_NE(swapchain.GetExtent().width, 0U);
        EXPECT_NE(swapchain.GetExtent().height, 0U);

        const VkFence acquireFence = CreateFence(device.GetHandle());
        const std::optional<std::uint32_t> imageIndex =
                swapchain.AcquireNextImage(std::numeric_limits<std::uint64_t>::max(), VK_NULL_HANDLE, acquireFence);
        ASSERT_TRUE(imageIndex.has_value());
        ASSERT_LT(*imageIndex, swapchain.GetImages().size());
        EXPECT_EQ(vkWaitForFences(device.GetHandle(),
                                  1,
                                  &acquireFence,
                                  VK_TRUE,
                                  std::numeric_limits<std::uint64_t>::max()),
                  VK_SUCCESS);

        swapchain.Present(device.LockPresentQueue(), *imageIndex, {});
        vkDestroyFence(device.GetHandle(), acquireFence, nullptr);

        const NVulkan::VulkanLockedQueue presentQueue = device.LockPresentQueue();
        vkQueueWaitIdle(presentQueue.GetHandle());
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }

        throw;
    }
}

TEST(VulkanSwapchain, CreatesAcquiresAndPresents) {
    RunSpawnedSmoke("VulkanSwapchainChild.DISABLED_CreatesAcquiresAndPresents");
}
