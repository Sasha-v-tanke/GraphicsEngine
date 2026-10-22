#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <spawn.h>
#include <string>
#include <string_view>
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

#include <graphics/vulkan/device.h>
#include <graphics/vulkan/frame_context.h>
#include <graphics/vulkan/glfw_surface.h>
#include <graphics/vulkan/instance.h>
#include <graphics/vulkan/physical_device.h>
#include <graphics/vulkan/submission_manager.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <window/window.h>
#include <window/window_config.h>
#include <window/window_runtime.h>
#include <window/window_size.h>
#include <window/window_type.h>

namespace {

constexpr std::uint64_t WaitTimeoutNs = 5'000'000'000;

bool IsEnvironmentFailure(const NCommon::Exception& exception) {
    const std::string& message = exception.GetMessage();

    return message.find("VK_ERROR_INCOMPATIBLE_DRIVER") != std::string::npos ||
           message.find("Failed to create GLFW window") != std::string::npos ||
           message.find("Failed to initialize GLFW") != std::string::npos ||
           message.find("Failed to get GLFW Vulkan instance extensions") != std::string::npos;
}

NWindow::WindowConfig MakeWindowConfig() {
    NWindow::WindowConfig windowConfig{NWindow::EWindowType::GLFW};
    windowConfig.Title = "GraphicsEngine Vulkan Submission Manager Test";
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
    FAIL() << "Vulkan submission manager smoke timed out";
}

struct SemaphoreDeleter {
    VkDevice Device = VK_NULL_HANDLE;

    void operator()(VkSemaphore semaphore) const noexcept {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(Device, semaphore, nullptr);
        }
    }
};

using UniqueSemaphore = std::unique_ptr<std::remove_pointer_t<VkSemaphore>, SemaphoreDeleter>;

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

VkCommandBufferSubmitInfo RecordEmptyCommandBuffer(VkCommandBuffer commandBuffer) {
    const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    EXPECT_EQ(vkBeginCommandBuffer(commandBuffer, &beginInfo), VK_SUCCESS);
    EXPECT_EQ(vkEndCommandBuffer(commandBuffer), VK_SUCCESS);

    return {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = commandBuffer,
    };
}

UniqueSemaphore CreateTimelineSemaphore(VkDevice device) {
    VkSemaphoreTypeCreateInfo timelineInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    const VkSemaphoreCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timelineInfo,
    };

    VkSemaphore semaphore = VK_NULL_HANDLE;
    EXPECT_EQ(vkCreateSemaphore(device, &createInfo, nullptr, &semaphore), VK_SUCCESS);

    return UniqueSemaphore{semaphore, SemaphoreDeleter{device}};
}

void SignalTimelineSemaphore(VkDevice device, VkSemaphore semaphore, std::uint64_t value) {
    const VkSemaphoreSignalInfo signalInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
            .semaphore = semaphore,
            .value = value,
    };

    ASSERT_EQ(vkSignalSemaphore(device, &signalInfo), VK_SUCCESS);
}

struct VulkanRuntime {
    NWindow::WindowRuntime WindowRuntime;
    NWindow::Window Window{MakeWindowConfig()};
    NVulkan::VulkanInstance Instance{MakeInstanceConfig()};
    NVulkan::NGlfw::VulkanSurface Surface{Instance, Window};
    NVulkan::VulkanPhysicalDeviceSelection PhysicalDevice =
            NVulkan::SelectVulkanPhysicalDevice(Instance, Surface.GetHandle());
    NVulkan::VulkanDevice Device{PhysicalDevice};

    [[nodiscard]] static NVulkan::VulkanInstanceConfig MakeInstanceConfig() {
        NVulkan::VulkanInstanceConfig instanceConfig;
        instanceConfig.ApplicationName = "GraphicsEngineVulkanSubmissionManagerTest";
        instanceConfig.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
        instanceConfig.ValidationMode = NVulkan::EValidationMode::EnabledIfAvailable;
        instanceConfig.EnableDebugUtils = true;
        return instanceConfig;
    }
};

TEST(VulkanSubmissionManagerChild, DISABLED_AssignsMonotonicTimelineValues) {
    try {
        VulkanRuntime runtime;
        NVulkan::VulkanSubmissionManager submissions{runtime.Device};
        NVulkan::VulkanFrameContextRing ring{runtime.Device, 3};

        NVulkan::VulkanFrameContext* firstContext = ring.TryAcquire({.FrameIndex = 0, .FrameSlotIndex = 0}, nullptr);
        NVulkan::VulkanFrameContext* secondContext = ring.TryAcquire({.FrameIndex = 1, .FrameSlotIndex = 1}, nullptr);
        NVulkan::VulkanFrameContext* thirdContext = ring.TryAcquire({.FrameIndex = 2, .FrameSlotIndex = 2}, nullptr);
        ASSERT_NE(firstContext, nullptr);
        ASSERT_NE(secondContext, nullptr);
        ASSERT_NE(thirdContext, nullptr);

        VkCommandBuffer firstBuffer = AllocateCommandBuffer(runtime.Device.GetHandle(), firstContext->GetCommandPool());
        VkCommandBuffer secondBuffer =
                AllocateCommandBuffer(runtime.Device.GetHandle(), secondContext->GetCommandPool());
        VkCommandBuffer thirdBuffer = AllocateCommandBuffer(runtime.Device.GetHandle(), thirdContext->GetCommandPool());
        const VkCommandBufferSubmitInfo firstBufferInfo = RecordEmptyCommandBuffer(firstBuffer);
        const VkCommandBufferSubmitInfo secondBufferInfo = RecordEmptyCommandBuffer(secondBuffer);
        const VkCommandBufferSubmitInfo thirdBufferInfo = RecordEmptyCommandBuffer(thirdBuffer);

        const NVulkan::VulkanQueueCompletion first = submissions.SubmitGraphics({
                .CommandBuffers = {firstBufferInfo},
        });
        const NVulkan::VulkanQueueCompletion second = submissions.SubmitGraphics({
                .CommandBuffers = {secondBufferInfo},
        });
        const NVulkan::VulkanQueueCompletion third = submissions.SubmitGraphics({
                .CommandBuffers = {thirdBufferInfo},
        });

        EXPECT_NE(first.Value, 0U);
        EXPECT_EQ(second.Value, first.Value + 1);
        EXPECT_EQ(third.Value, second.Value + 1);

        submissions.Wait(third, WaitTimeoutNs);

        EXPECT_TRUE(submissions.IsCompleted(first));
        EXPECT_TRUE(submissions.IsCompleted(second));
        EXPECT_TRUE(submissions.IsCompleted(third));
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }

        throw;
    }
}

TEST(VulkanSubmissionManagerChild, DISABLED_BlocksFrameReuseUntilTimelineCompletion) {
    try {
        VulkanRuntime runtime;
        NVulkan::VulkanSubmissionManager submissions{runtime.Device};
        NVulkan::VulkanFrameContextRing ring{runtime.Device, 1};
        const UniqueSemaphore gate = CreateTimelineSemaphore(runtime.Device.GetHandle());
        const VkSemaphoreSubmitInfo waitSemaphore{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = gate.get(),
                .value = 1,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };

        NVulkan::VulkanFrameContext* context = ring.TryAcquire({.FrameIndex = 0, .FrameSlotIndex = 0}, nullptr);
        ASSERT_NE(context, nullptr);
        VkCommandBuffer commandBuffer = AllocateCommandBuffer(runtime.Device.GetHandle(), context->GetCommandPool());
        const VkCommandBufferSubmitInfo commandBufferInfo = RecordEmptyCommandBuffer(commandBuffer);

        const NVulkan::VulkanQueueCompletion completion = submissions.SubmitGraphics({
                .WaitSemaphores = {waitSemaphore},
                .CommandBuffers = {commandBufferInfo},
        });
        ring.MarkSubmitted({.FrameIndex = 0, .FrameSlotIndex = 0}, completion.Value);

        EXPECT_FALSE(submissions.IsCompleted(completion));
        EXPECT_EQ(ring.TryAcquire({.FrameIndex = 1, .FrameSlotIndex = 0},
                                  [&](std::uint64_t value) { return submissions.IsCompleted({.Value = value}); }),
                  nullptr);

        SignalTimelineSemaphore(runtime.Device.GetHandle(), gate.get(), 1);
        submissions.Wait(completion, WaitTimeoutNs);

        NVulkan::VulkanFrameContext* recycled =
                ring.TryAcquire({.FrameIndex = 1, .FrameSlotIndex = 0},
                                [&](std::uint64_t value) { return submissions.IsCompleted({.Value = value}); });
        ASSERT_NE(recycled, nullptr);
        EXPECT_EQ(recycled, context);
        EXPECT_EQ(recycled->GetFrameIndex(), 1U);
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }

        throw;
    }
}

TEST(VulkanSubmissionManagerChild, DISABLED_SupportsMultipleFrameSlotsAndShutdownInFlight) {
    try {
        VulkanRuntime runtime;
        NVulkan::VulkanSubmissionManager submissions{runtime.Device};
        NVulkan::VulkanFrameContextRing ring{runtime.Device, 2};

        NVulkan::VulkanFrameContext* first = ring.TryAcquire({.FrameIndex = 0, .FrameSlotIndex = 0}, nullptr);
        NVulkan::VulkanFrameContext* second = ring.TryAcquire({.FrameIndex = 1, .FrameSlotIndex = 1}, nullptr);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        VkCommandBuffer firstBuffer = AllocateCommandBuffer(runtime.Device.GetHandle(), first->GetCommandPool());
        VkCommandBuffer secondBuffer = AllocateCommandBuffer(runtime.Device.GetHandle(), second->GetCommandPool());
        const VkCommandBufferSubmitInfo firstBufferInfo = RecordEmptyCommandBuffer(firstBuffer);
        const VkCommandBufferSubmitInfo secondBufferInfo = RecordEmptyCommandBuffer(secondBuffer);

        const NVulkan::VulkanQueueCompletion firstCompletion = submissions.SubmitGraphics({
                .CommandBuffers = {firstBufferInfo},
        });
        const NVulkan::VulkanQueueCompletion secondCompletion = submissions.SubmitGraphics({
                .CommandBuffers = {secondBufferInfo},
        });

        EXPECT_LT(firstCompletion.Value, secondCompletion.Value);
        ring.MarkSubmitted({.FrameIndex = 0, .FrameSlotIndex = 0}, firstCompletion.Value);
        ring.MarkSubmitted({.FrameIndex = 1, .FrameSlotIndex = 1}, secondCompletion.Value);
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }

        throw;
    }
}

TEST(VulkanSubmissionManager, AssignsMonotonicTimelineValues) {
    RunSpawnedSmoke("VulkanSubmissionManagerChild.DISABLED_AssignsMonotonicTimelineValues");
}

TEST(VulkanSubmissionManager, BlocksFrameReuseUntilTimelineCompletion) {
    RunSpawnedSmoke("VulkanSubmissionManagerChild.DISABLED_BlocksFrameReuseUntilTimelineCompletion");
}

TEST(VulkanSubmissionManager, SupportsMultipleFrameSlotsAndShutdownInFlight) {
    RunSpawnedSmoke("VulkanSubmissionManagerChild.DISABLED_SupportsMultipleFrameSlotsAndShutdownInFlight");
}

} // namespace
