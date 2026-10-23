#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <spawn.h>
#include <string>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

#include <sys/wait.h>

#if defined(__APPLE__)
    #include <crt_externs.h>

    #include <mach-o/dyld.h>
#else
extern char** environ;
#endif

#include <glfw.h>

#include <graphics/vulkan/clear_present_path.h>
#include <graphics/vulkan/device.h>
#include <graphics/vulkan/glfw_surface.h>
#include <graphics/vulkan/instance.h>
#include <graphics/vulkan/physical_device.h>
#include <graphics/vulkan/resource_state.h>
#include <graphics/vulkan/swapchain.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <window/engine/GLFW/window_engine.h>
#include <window/internal/window_engine_access.h>
#include <window/window.h>
#include <window/window_config.h>
#include <window/window_runtime.h>
#include <window/window_size.h>
#include <window/window_type.h>

namespace {

using NVulkan::EVulkanSwapchainAcquireStatus;
using NVulkan::EVulkanSwapchainPresentStatus;
using NVulkan::MakeVulkanSwapchainPlan;
using NVulkan::VulkanSwapchainPlan;
using NVulkan::VulkanSwapchainSupport;

struct SemaphoreDeleter {
    VkDevice Device = VK_NULL_HANDLE;

    void operator()(VkSemaphore semaphore) const noexcept {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(Device, semaphore, nullptr);
        }
    }
};

struct FenceDeleter {
    VkDevice Device = VK_NULL_HANDLE;

    void operator()(VkFence fence) const noexcept {
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(Device, fence, nullptr);
        }
    }
};

struct CommandPoolDeleter {
    VkDevice Device = VK_NULL_HANDLE;

    void operator()(VkCommandPool commandPool) const noexcept {
        if (commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(Device, commandPool, nullptr);
        }
    }
};

using UniqueSemaphore = std::unique_ptr<std::remove_pointer_t<VkSemaphore>, SemaphoreDeleter>;
using UniqueFence = std::unique_ptr<std::remove_pointer_t<VkFence>, FenceDeleter>;
using UniqueCommandPool = std::unique_ptr<std::remove_pointer_t<VkCommandPool>, CommandPoolDeleter>;

class SwapchainResultOverride {
public:
    SwapchainResultOverride() {
        OriginalAcquire = vkAcquireNextImageKHR;
        OriginalPresent = vkQueuePresentKHR;
        vkAcquireNextImageKHR = Acquire;
        vkQueuePresentKHR = Present;
    }

    ~SwapchainResultOverride() {
        vkAcquireNextImageKHR = OriginalAcquire;
        vkQueuePresentKHR = OriginalPresent;
        NextAcquire.reset();
        NextPresent.reset();
    }

    inline static std::optional<VkResult> NextAcquire;
    inline static std::optional<VkResult> NextPresent;

private:
    static VKAPI_ATTR VkResult VKAPI_CALL Acquire(VkDevice device,
                                                  VkSwapchainKHR swapchain,
                                                  std::uint64_t timeout,
                                                  VkSemaphore semaphore,
                                                  VkFence fence,
                                                  std::uint32_t* imageIndex) {
        if (NextAcquire == VK_ERROR_OUT_OF_DATE_KHR) {
            NextAcquire.reset();
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        const VkResult result = OriginalAcquire(device, swapchain, timeout, semaphore, fence, imageIndex);
        if (NextAcquire.has_value() && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) {
            const auto overridden = *NextAcquire;
            NextAcquire.reset();
            return overridden;
        }
        return result;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue queue, const VkPresentInfoKHR* info) {
        const VkResult result = OriginalPresent(queue, info);
        if (NextPresent.has_value() && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) {
            const auto overridden = *NextPresent;
            NextPresent.reset();
            return overridden;
        }
        return result;
    }

    inline static PFN_vkAcquireNextImageKHR OriginalAcquire = nullptr;
    inline static PFN_vkQueuePresentKHR OriginalPresent = nullptr;
};

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
                            .supportedUsageFlags =
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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

UniqueSemaphore CreateSemaphore(VkDevice device) {
    const VkSemaphoreCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    VkSemaphore semaphore = VK_NULL_HANDLE;
    EXPECT_EQ(vkCreateSemaphore(device, &createInfo, nullptr, &semaphore), VK_SUCCESS);

    return UniqueSemaphore{semaphore, SemaphoreDeleter{device}};
}

UniqueFence CreateFence(VkDevice device) {
    const VkFenceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };

    VkFence fence = VK_NULL_HANDLE;
    EXPECT_EQ(vkCreateFence(device, &createInfo, nullptr, &fence), VK_SUCCESS);

    return UniqueFence{fence, FenceDeleter{device}};
}

UniqueCommandPool CreateCommandPool(VkDevice device, std::uint32_t queueFamilyIndex) {
    const VkCommandPoolCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queueFamilyIndex,
    };

    VkCommandPool commandPool = VK_NULL_HANDLE;
    EXPECT_EQ(vkCreateCommandPool(device, &createInfo, nullptr, &commandPool), VK_SUCCESS);

    return UniqueCommandPool{commandPool, CommandPoolDeleter{device}};
}

VkCommandBuffer AllocateCommandBuffer(VkDevice device, VkCommandPool commandPool) {
    const VkCommandBufferAllocateInfo allocateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
    };

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    EXPECT_EQ(vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer), VK_SUCCESS);

    return commandBuffer;
}

void RecordPresentTransition(VkCommandBuffer commandBuffer, VkImage image) {
    const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    ASSERT_EQ(vkBeginCommandBuffer(commandBuffer, &beginInfo), VK_SUCCESS);

    NVulkan::VulkanResourceStateTracker tracker;
    tracker.ImportImage(image,
                        {
                                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .baseMipLevel = 0,
                                .levelCount = 1,
                                .baseArrayLayer = 0,
                                .layerCount = 1,
                        },
                        NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::Undefined));
    const std::optional<VkImageMemoryBarrier2> barrier = tracker.TransitionImage({
            .Image = image,
            .Usage = NVulkan::EVulkanResourceUsage::Present,
            .SubresourceRange =
                    {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .baseMipLevel = 0,
                            .levelCount = 1,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                    },
    });
    ASSERT_TRUE(barrier.has_value());
    const VkDependencyInfo dependencyInfo{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &*barrier,
    };
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

    ASSERT_EQ(vkEndCommandBuffer(commandBuffer), VK_SUCCESS);
}

void SubmitPresentTransition(const NVulkan::VulkanDevice& device,
                             VkCommandBuffer commandBuffer,
                             VkSemaphore imageAvailable,
                             VkSemaphore presentReady,
                             VkFence fence) {
    const VkSemaphoreSubmitInfo waitSemaphore{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = imageAvailable,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkCommandBufferSubmitInfo commandBufferInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = commandBuffer,
    };
    const VkSemaphoreSubmitInfo signalSemaphore{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = presentReady,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkSubmitInfo2 submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = 1,
            .pWaitSemaphoreInfos = &waitSemaphore,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &commandBufferInfo,
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos = &signalSemaphore,
    };

    const NVulkan::VulkanLockedQueue graphicsQueue = device.LockGraphicsQueue();
    ASSERT_EQ(vkQueueSubmit2(graphicsQueue.GetHandle(), 1, &submitInfo, fence), VK_SUCCESS);
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

void RunSpawnedSmoke(std::string_view filter, int maxAttempts = 100) {
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
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
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
    FAIL() << "Vulkan swapchain smoke timed out";
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

        const UniqueSemaphore imageAvailable = CreateSemaphore(device.GetHandle());
        const UniqueSemaphore presentReady = CreateSemaphore(device.GetHandle());
        const UniqueFence submitted = CreateFence(device.GetHandle());
        const UniqueCommandPool commandPool =
                CreateCommandPool(device.GetHandle(), physicalDevice.GraphicsQueueFamilyIndex);

        const NVulkan::VulkanSwapchainAcquireResult acquire =
                swapchain.AcquireNextImage(imageAvailable.get(), VK_NULL_HANDLE, UINT64_MAX);
        ASSERT_TRUE(acquire.Status == EVulkanSwapchainAcquireStatus::ACQUIRED ||
                    acquire.Status == EVulkanSwapchainAcquireStatus::SUBOPTIMAL);
        ASSERT_LT(acquire.ImageIndex, swapchain.GetImages().size());

        const VkCommandBuffer commandBuffer = AllocateCommandBuffer(device.GetHandle(), commandPool.get());
        RecordPresentTransition(commandBuffer, swapchain.GetImages()[acquire.ImageIndex].Handle);
        SubmitPresentTransition(device, commandBuffer, imageAvailable.get(), presentReady.get(), submitted.get());
        const VkFence submittedFence = submitted.get();
        ASSERT_EQ(vkWaitForFences(device.GetHandle(), 1, &submittedFence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

        {
            const NVulkan::VulkanLockedQueue presentQueue = device.LockPresentQueue();
            const EVulkanSwapchainPresentStatus presentStatus =
                    swapchain.Present(presentQueue, acquire.ImageIndex, {presentReady.get()});
            EXPECT_TRUE(presentStatus == EVulkanSwapchainPresentStatus::PRESENTED ||
                        presentStatus == EVulkanSwapchainPresentStatus::SUBOPTIMAL);
        }

        ASSERT_EQ(vkDeviceWaitIdle(device.GetHandle()), VK_SUCCESS);
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

TEST(VulkanClearPresentPathChild, DISABLED_ClearsAndPresentsRepeatedFrames) {
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{MakeWindowConfig()};

        std::vector<std::string> validationErrors;
        std::mutex validationMutex;
        NVulkan::VulkanInstanceConfig instanceConfig;
        instanceConfig.ApplicationName = "GraphicsEngineVulkanClearPresentPathTest";
        instanceConfig.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
        instanceConfig.ValidationMode = NVulkan::EValidationMode::Required;
        instanceConfig.EnableDebugUtils = true;
        instanceConfig.DebugMessageHandler = [&](const NVulkan::VulkanDebugMessage& message) {
            if ((message.Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                const std::lock_guard lock{validationMutex};
                validationErrors.push_back(message.Message);
            }
        };

        NVulkan::VulkanInstance instance{instanceConfig};
        const NVulkan::NGlfw::VulkanSurface surface{instance, window};
        const NVulkan::VulkanPhysicalDeviceSelection physicalDevice =
                NVulkan::SelectVulkanPhysicalDevice(instance, surface.GetHandle());
        const NVulkan::VulkanDevice device{physicalDevice};
        NVulkan::VulkanSwapchain swapchain{device, physicalDevice, surface.GetHandle(), window.GetFramebufferSize()};
        {
            NVulkan::VulkanSwapchain suspended{device, physicalDevice, surface.GetHandle(), {.Width = 0, .Height = 0}};
            NVulkan::VulkanClearPresentPath path{device, suspended, 1};
            const NVulkan::VulkanClearPresentResult result = path.Render(0, {.float32 = {0, 0, 0, 1}});
            EXPECT_EQ(result.Status, NVulkan::EVulkanClearPresentStatus::SUSPENDED);
            EXPECT_FALSE(result.Completion.has_value());
            EXPECT_TRUE(path.PollCompletedFrames().empty());
        }
        for (const std::size_t activeSlots: {1U, 3U}) {
            NVulkan::VulkanClearPresentPath path{device, swapchain, activeSlots};

            std::uint64_t frameIndex = 0;
            std::vector<NVulkan::VulkanFrameCompletionEvent> completedFrames;
            for (std::size_t attempts = 0; frameIndex < 6 && attempts < 600; ++attempts) {
                const NVulkan::VulkanClearPresentResult result =
                        path.Render(frameIndex, {.float32 = {0.1F * static_cast<float>(frameIndex), 0.2F, 0.3F, 1.0F}});
                if (result.Completion.has_value()) {
                    EXPECT_NE(result.Completion->Value, 0U);
                    ++frameIndex;
                } else {
                    ASSERT_EQ(result.Status, NVulkan::EVulkanClearPresentStatus::NOT_READY);
                    std::this_thread::sleep_for(std::chrono::milliseconds{10});
                }
                ASSERT_NE(result.Status, NVulkan::EVulkanClearPresentStatus::OUT_OF_DATE);
                ASSERT_NE(result.Status, NVulkan::EVulkanClearPresentStatus::SUSPENDED);

                std::vector<NVulkan::VulkanFrameCompletionEvent> completed = path.PollCompletedFrames();
                completedFrames.insert(completedFrames.end(), completed.begin(), completed.end());
            }
            EXPECT_EQ(frameIndex, 6U);

            for (std::size_t attempts = 0; completedFrames.size() < 6 && attempts < 600; ++attempts) {
                std::vector<NVulkan::VulkanFrameCompletionEvent> completed = path.PollCompletedFrames();
                completedFrames.insert(completedFrames.end(), completed.begin(), completed.end());
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            ASSERT_EQ(completedFrames.size(), 6U);
            for (std::size_t index = 0; index < completedFrames.size(); ++index) {
                EXPECT_EQ(completedFrames[index].FrameIndex, index);
                EXPECT_NE(completedFrames[index].Completion.Value, 0U);
            }
        }
        const std::lock_guard lock{validationMutex};
        EXPECT_TRUE(validationErrors.empty());
        for (const std::string& error: validationErrors) {
            ADD_FAILURE() << error;
        }
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }
        throw;
    }
}

TEST(VulkanClearPresentPath, ClearsAndPresentsRepeatedFrames) {
    RunSpawnedSmoke("VulkanClearPresentPathChild.DISABLED_ClearsAndPresentsRepeatedFrames");
}

TEST(VulkanClearPresentPathChild, DISABLED_RecreatesAcrossResizeAndSuspension) {
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{MakeWindowConfig()};
        auto& native =
                dynamic_cast<NWindow::NEngine::NGlfw::IGlfwWindowEngine&>(NWindow::NInternal::GetWindowEngine(window))
                        .GetGlfwWindow();
        std::mutex validationMutex;
        std::vector<std::string> validationErrors;
        NVulkan::VulkanInstanceConfig config;
        config.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
        config.ValidationMode = NVulkan::EValidationMode::Required;
        config.EnableDebugUtils = true;
        config.DebugMessageHandler = [&](const NVulkan::VulkanDebugMessage& message) {
            if ((message.Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                const std::lock_guard lock{validationMutex};
                validationErrors.push_back(message.Message);
            }
        };
        NVulkan::VulkanInstance instance{config};
        const NVulkan::NGlfw::VulkanSurface surface{instance, window};
        const auto physicalDevice = NVulkan::SelectVulkanPhysicalDevice(instance, surface.GetHandle());
        {
            const NVulkan::VulkanDevice device{physicalDevice};
            const auto deviceHandle = device.GetHandle();
            const SwapchainResultOverride resultOverride;
            NVulkan::VulkanSwapchain swapchain{device, physicalDevice, surface.GetHandle(), {0, 0}};
            for (const std::size_t slots: {1U, 3U}) {
                NVulkan::VulkanClearPresentPath path{device, swapchain, slots};
                std::uint64_t nextFrame = 0;
                auto lastStatus = NVulkan::EVulkanClearPresentStatus::NOT_READY;
                std::vector<NVulkan::VulkanFrameCompletionEvent> completed;
                auto collect = [&] {
                    auto events = path.PollCompletedFrames();
                    completed.insert(completed.end(), events.begin(), events.end());
                };
                auto renderFrames = [&](std::uint64_t target) {
                    for (int attempt = 0; nextFrame < target && attempt < 1000; ++attempt) {
                        window.ProcessEvents();
                        path.SetFramebufferSize(window.GetFramebufferSize());
                        const auto result = path.Render(nextFrame, {.float32 = {0.2F, 0.3F, 0.4F, 1.0F}});
                        lastStatus = result.Status;
                        if (result.Completion.has_value()) {
                            ++nextFrame;
                        } else {
                            EXPECT_NE(result.Status, NVulkan::EVulkanClearPresentStatus::PRESENTED);
                            std::this_thread::sleep_for(std::chrono::milliseconds{1});
                        }
                    }
                    EXPECT_EQ(nextFrame, target);
                };
                path.SetFramebufferSize({0, 0});
                EXPECT_EQ(path.Render(0, {}).Status, NVulkan::EVulkanClearPresentStatus::SUSPENDED);
                for (int cycle = 0; cycle < 4; ++cycle) {
                    window.SetSize({96 + cycle * 16, 80 + cycle * 16});
                    renderFrames(nextFrame + 3);
                    const auto oldSwapchain = swapchain.GetHandle();
                    EXPECT_NE(oldSwapchain, VK_NULL_HANDLE);
                    glfwIconifyWindow(&native);
                    window.ProcessEvents();
                    path.SetFramebufferSize({0, 0});
                    for (int suspendedFrame = 0; suspendedFrame < 3; ++suspendedFrame) {
                        const auto result = path.Render(nextFrame, {});
                        EXPECT_EQ(result.Status, NVulkan::EVulkanClearPresentStatus::SUSPENDED);
                        EXPECT_FALSE(result.Completion.has_value());
                    }
                    EXPECT_EQ(swapchain.GetHandle(), oldSwapchain);
                    collect();
                    glfwRestoreWindow(&native);
                    window.ProcessEvents();
                    renderFrames(nextFrame + 2);
                    EXPECT_FALSE(swapchain.IsSuspended());
                    EXPECT_FALSE(swapchain.GetImages().empty());
                    EXPECT_EQ(device.GetHandle(), deviceHandle);
                    collect();
                }
                for (const auto acquireResult: {VK_ERROR_OUT_OF_DATE_KHR, VK_SUBOPTIMAL_KHR}) {
                    const auto previous = swapchain.GetHandle();
                    SwapchainResultOverride::NextAcquire = acquireResult;
                    renderFrames(nextFrame + 1);
                    EXPECT_FALSE(SwapchainResultOverride::NextAcquire.has_value());
                    if (acquireResult == VK_SUBOPTIMAL_KHR) {
                        renderFrames(nextFrame + 1);
                    }
                    EXPECT_NE(swapchain.GetHandle(), previous);
                }
                for (const auto presentResult: {VK_ERROR_OUT_OF_DATE_KHR, VK_SUBOPTIMAL_KHR}) {
                    SwapchainResultOverride::NextPresent = presentResult;
                    renderFrames(nextFrame + 1);
                    EXPECT_FALSE(SwapchainResultOverride::NextPresent.has_value());
                    EXPECT_EQ(lastStatus,
                              presentResult == VK_ERROR_OUT_OF_DATE_KHR
                                      ? NVulkan::EVulkanClearPresentStatus::OUT_OF_DATE
                                      : NVulkan::EVulkanClearPresentStatus::PRESENTED);
                    const auto previous = swapchain.GetHandle();
                    renderFrames(nextFrame + 1);
                    EXPECT_NE(swapchain.GetHandle(), previous);
                }
                for (int attempt = 0; completed.size() < nextFrame && attempt < 1000; ++attempt) {
                    collect();
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                ASSERT_EQ(completed.size(), nextFrame);
                for (std::size_t index = 0; index < completed.size(); ++index) {
                    EXPECT_EQ(completed[index].FrameIndex, index);
                    EXPECT_GT(completed[index].Completion.Value, 0U);
                }
            }
        }
        const std::lock_guard lock{validationMutex};
        EXPECT_TRUE(validationErrors.empty());
        for (const auto& error: validationErrors) {
            ADD_FAILURE() << error;
        }
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }
        throw;
    }
}

TEST(VulkanClearPresentPath, RecreatesAcrossResizeAndSuspension) {
    RunSpawnedSmoke("VulkanClearPresentPathChild.DISABLED_RecreatesAcrossResizeAndSuspension", 300);
}
