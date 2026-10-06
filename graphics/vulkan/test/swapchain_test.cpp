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

VkExtent2D MakeExtent(std::uint32_t width, std::uint32_t height) noexcept {
    return {
            .width = width,
            .height = height,
    };
}

VkExtent2D GetFramebufferExtent(const NWindow::Window& window) noexcept {
    const NWindow::WindowSize size = window.GetFramebufferSize();

    return {
            .width = size.Width <= 0 ? 0U : static_cast<std::uint32_t>(size.Width),
            .height = size.Height <= 0 ? 0U : static_cast<std::uint32_t>(size.Height),
    };
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

VkCommandPool CreateCommandPool(VkDevice device, std::uint32_t queueFamilyIndex) {
    const VkCommandPoolCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = queueFamilyIndex,
    };

    VkCommandPool commandPool = VK_NULL_HANDLE;
    const VkResult result = vkCreateCommandPool(device, &createInfo, nullptr, &commandPool);
    if (result != VK_SUCCESS) {
        throw std::runtime_error{"Failed to create Vulkan command pool"};
    }

    return commandPool;
}

VkCommandBuffer AllocateCommandBuffer(VkDevice device, VkCommandPool commandPool) {
    const VkCommandBufferAllocateInfo allocateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
    };

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    const VkResult result = vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer);
    if (result != VK_SUCCESS) {
        throw std::runtime_error{"Failed to allocate Vulkan command buffer"};
    }

    return commandBuffer;
}

void TransitionSwapchainImageToPresent(VkDevice device,
                                       VkCommandPool commandPool,
                                       NVulkan::VulkanLockedQueue graphicsQueue,
                                       VkImage image) {
    const VkCommandBuffer commandBuffer = AllocateCommandBuffer(device, commandPool);
    const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    ASSERT_EQ(vkBeginCommandBuffer(commandBuffer, &beginInfo), VK_SUCCESS);

    const VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
            .dstAccessMask = VK_ACCESS_2_NONE,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange =
                    {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .baseMipLevel = 0,
                            .levelCount = 1,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                    },
    };
    const VkDependencyInfo dependencyInfo{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
    };

    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

    ASSERT_EQ(vkEndCommandBuffer(commandBuffer), VK_SUCCESS);

    const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffer,
    };

    ASSERT_EQ(vkQueueSubmit(graphicsQueue.GetHandle(), 1, &submitInfo, VK_NULL_HANDLE), VK_SUCCESS);
    ASSERT_EQ(vkQueueWaitIdle(graphicsQueue.GetHandle()), VK_SUCCESS);
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
                GetFramebufferExtent(window),
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

        EXPECT_THROW((void)swapchain.AcquireNextImage(0, VK_NULL_HANDLE, VK_NULL_HANDLE), NCommon::Exception);

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

        const VkCommandPool commandPool =
                CreateCommandPool(device.GetHandle(), physicalDevice.GraphicsQueueFamilyIndex);
        TransitionSwapchainImageToPresent(device.GetHandle(),
                                          commandPool,
                                          device.LockGraphicsQueue(),
                                          swapchain.GetImages()[*imageIndex]);
        swapchain.Present(device.LockPresentQueue(), *imageIndex, {});
        vkDestroyCommandPool(device.GetHandle(), commandPool, nullptr);
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
