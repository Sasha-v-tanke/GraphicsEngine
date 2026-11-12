#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
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
#include <graphics/vulkan/memory.h>
#include <graphics/vulkan/physical_device.h>
#include <graphics/vulkan/submission_manager.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <resources/resource_manager.h>
#include <tests/common/test_error.h>
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
            if (WEXITSTATUS(status) == 77) {
                GTEST_SKIP() << "Required Vulkan environment unavailable; see child diagnostic";
            }
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
    std::shared_ptr<std::atomic_uint32_t> ValidationErrors = std::make_shared<std::atomic_uint32_t>(0);
    NVulkan::VulkanInstance Instance;
    NVulkan::NGlfw::VulkanSurface Surface{Instance, Window};
    NVulkan::VulkanPhysicalDeviceSelection PhysicalDevice =
            NVulkan::SelectVulkanPhysicalDevice(Instance, Surface.GetHandle());
    NVulkan::VulkanDevice Device{PhysicalDevice};

    explicit VulkanRuntime(bool requiredValidation = false)
        : Instance(MakeInstanceConfig(requiredValidation, ValidationErrors)) {
    }

    [[nodiscard]] static NVulkan::VulkanInstanceConfig
    MakeInstanceConfig(bool requiredValidation, const std::shared_ptr<std::atomic_uint32_t>& errors) {
        NVulkan::VulkanInstanceConfig instanceConfig;
        instanceConfig.ApplicationName = "GraphicsEngineVulkanSubmissionManagerTest";
        instanceConfig.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
        instanceConfig.ValidationMode =
                requiredValidation ? NVulkan::EValidationMode::Required : NVulkan::EValidationMode::EnabledIfAvailable;
        instanceConfig.EnableDebugUtils = true;
        instanceConfig.DebugMessageHandler = [errors](const NVulkan::VulkanDebugMessage& message) {
            if ((message.Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                ++*errors;
                std::cerr << message.Message << '\n';
            }
        };
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

struct ReleaseGate {
    VkDevice Device;
    VkSemaphore Gate;
    ~ReleaseGate() {
        std::uint64_t current = 0;
        if (vkGetSemaphoreCounterValue(Device, Gate, &current) == VK_SUCCESS && current < 2) {
            const VkSemaphoreSignalInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                               .semaphore = Gate,
                                               .value = 2};
            static_cast<void>(vkSignalSemaphore(Device, &signal));
        }
    }
};

void HandleRequiredEnvironment(const NCommon::Exception& exception) {
    if (IsEnvironmentFailure(exception) ||
        exception.GetMessage().find("Required Vulkan validation layer") != std::string::npos) {
        std::cerr << "Vulkan submission retention SKIP: " << exception.GetMessage() << '\n';
        std::exit(77);
    }
}

TEST(VulkanSubmissionManagerChild, DISABLED_RetainsAllocationUntilLastActualConsumer) {
    try {
        VulkanRuntime runtime{true};
        ASSERT_TRUE(runtime.Instance.IsValidationEnabled());
        for (bool shutdown: {false, true}) {
            {
                NVulkan::VulkanResourceStateTracker states;
                NVulkan::VulkanMemoryAllocator memory{runtime.Instance.GetHandle(),
                                                      runtime.PhysicalDevice.Handle,
                                                      runtime.Device,
                                                      states};
                const auto gate = CreateTimelineSemaphore(runtime.Device.GetHandle());
                NVulkan::VulkanSubmissionManager submissions{runtime.Device};
                NVulkan::VulkanFrameContextRing ring{runtime.Device, 2};
                ReleaseGate release{runtime.Device.GetHandle(), gate.get()};
                auto allocation = NVulkan::CreateVulkanBuffer(
                        memory,
                        {.SizeBytes = 64,
                         .Usage =
                                 NGraphics::EBufferUsage::TransferDestination | NGraphics::EBufferUsage::TransferSource,
                         .Access = NGraphics::EBufferAccess::GpuRead | NGraphics::EBufferAccess::GpuWrite});
                VkBuffer native = allocation.GetBuffer();
                std::weak_ptr<void> lifetime = allocation.GetRetentionToken();
                NResources::ResourceManager resources;
                const auto handle = resources.Request<int>({"test", "native-gpu-retention"});
                const auto operation = resources.BeginLoading(handle);
                const auto cpu = std::make_shared<const int>(7);
                ASSERT_TRUE(resources.TryAdvance(operation,
                                                 NResources::EResourceState::LOADING,
                                                 NResources::EResourceState::CPU_READY,
                                                 cpu));
                ASSERT_TRUE(resources.TryAdvance(operation,
                                                 NResources::EResourceState::CPU_READY,
                                                 NResources::EResourceState::READY,
                                                 cpu,
                                                 allocation.GetRetentionToken()));
                auto lease = resources.TryAcquire(handle);
                ASSERT_TRUE(lease);
                std::vector<std::shared_ptr<const void>> uses{lease->GetGpuRepresentation()};
                NVulkan::VulkanQueueCompletion completions[2];
                for (std::uint64_t index = 0; index < 2; ++index) {
                    auto* context = ring.TryAcquire({.FrameIndex = index, .FrameSlotIndex = index}, nullptr);
                    ASSERT_NE(context, nullptr);
                    VkCommandBuffer command =
                            AllocateCommandBuffer(runtime.Device.GetHandle(), context->GetCommandPool());
                    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                    ASSERT_EQ(vkBeginCommandBuffer(command, &begin), VK_SUCCESS);
                    if (index != 0) {
                        const VkBufferMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                                             .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                                             .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                             .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                                             .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                             .buffer = native,
                                                             .size = VK_WHOLE_SIZE};
                        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                          .bufferMemoryBarrierCount = 1,
                                                          .pBufferMemoryBarriers = &barrier};
                        vkCmdPipelineBarrier2(command, &dependency);
                    }
                    vkCmdFillBuffer(command, native, 0, VK_WHOLE_SIZE, static_cast<std::uint32_t>(index));
                    ASSERT_EQ(vkEndCommandBuffer(command), VK_SUCCESS);
                    const VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                                     .semaphore = gate.get(),
                                                     .value = index + 1,
                                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
                    const VkCommandBufferSubmitInfo recorded{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                             .commandBuffer = command};
                    completions[index] = submissions.SubmitGraphics(
                            {.WaitSemaphores = {wait}, .CommandBuffers = {recorded}, .ResourceUses = uses});
                    ring.MarkSubmitted({.FrameIndex = index, .FrameSlotIndex = index}, completions[index].Value);
                }
                uses.clear();
                allocation = {};
                lease.reset();
                resources.RequestUnload(handle);
                resources.CompleteUnload(handle);
                EXPECT_FALSE(resources.TryAcquire(handle));
                EXPECT_FALSE(lifetime.expired());
                EXPECT_FALSE(submissions.IsCompleted(completions[0]));
                SignalTimelineSemaphore(runtime.Device.GetHandle(), gate.get(), 1);
                submissions.Wait(completions[0], WaitTimeoutNs);
                submissions.ReleaseCompleted();
                EXPECT_FALSE(lifetime.expired());
                EXPECT_FALSE(submissions.IsCompleted(completions[1]));
                if (shutdown) {
                    std::jthread signal([&] { SignalTimelineSemaphore(runtime.Device.GetHandle(), gate.get(), 2); });
                    EXPECT_EQ(submissions.Shutdown(), NVulkan::EVulkanSubmissionShutdown::DRAINED);
                    signal.join();
                } else {
                    SignalTimelineSemaphore(runtime.Device.GetHandle(), gate.get(), 2);
                    // A completion observer may live on another host thread. It cannot run
                    // resource-state/VMA destruction belonging to the recording owner lane.
                    std::jthread observer([&] {
                        submissions.Wait(completions[1], WaitTimeoutNs);
                        EXPECT_TRUE(submissions.IsCompleted(completions[1]));
                    });
                    observer.join();
                    EXPECT_FALSE(lifetime.expired());
                    submissions.ReleaseCompleted();
                }
                EXPECT_TRUE(lifetime.expired());
            }
        }
        EXPECT_EQ(runtime.ValidationErrors->load(), 0U);
    } catch (const NCommon::Exception& exception) {
        HandleRequiredEnvironment(exception);
        throw;
    }
}

class SubmissionStatusOverride {
public:
    SubmissionStatusOverride() {
        OriginalSubmit = vkQueueSubmit2;
        OriginalIdle = vkQueueWaitIdle;
        vkQueueSubmit2 = Submit;
        vkQueueWaitIdle = Idle;
    }
    ~SubmissionStatusOverride() {
        vkQueueSubmit2 = OriginalSubmit;
        vkQueueWaitIdle = OriginalIdle;
        NextSubmit.reset();
        NextIdle.reset();
    }
    inline static std::optional<VkResult> NextSubmit;
    inline static std::optional<VkResult> NextIdle;

private:
    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue,
                                                 std::uint32_t count,
                                                 const VkSubmitInfo2* submits,
                                                 VkFence fence) {
        if (NextSubmit) {
            const auto result = *NextSubmit;
            NextSubmit.reset();
            return result;
        }
        return OriginalSubmit(queue, count, submits, fence);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Idle(VkQueue queue) {
        if (NextIdle) {
            const auto result = *NextIdle;
            NextIdle.reset();
            return result;
        }
        return OriginalIdle(queue);
    }
    inline static PFN_vkQueueSubmit2 OriginalSubmit = nullptr;
    inline static PFN_vkQueueWaitIdle OriginalIdle = nullptr;
};

TEST(VulkanSubmissionManagerChild, DISABLED_RollbackShutdownRetryAndDeviceLoss) {
    try {
        VulkanRuntime runtime{true};
        {
            NVulkan::VulkanSubmissionManager submissions{runtime.Device};
            SubmissionStatusOverride override;
            struct ReentryState {
                NVulkan::VulkanSubmissionManager* Owner = nullptr;
                NVulkan::VulkanQueueCompletion Last;
                bool Completed = false;
            };
            auto reentry = std::make_shared<ReentryState>();
            reentry->Owner = &submissions;
            const auto deleter = [reentry](const int* payload) noexcept {
                delete payload;
                try {
                    reentry->Completed = reentry->Owner->IsCompleted(reentry->Last);
                } catch (...) {
                    reentry->Completed = false;
                }
            };
            auto gpu = std::shared_ptr<const int>{new int(1), deleter};
            const std::weak_ptr<const int> lifetime = gpu;
            const auto first = submissions.SubmitGraphics({.ResourceUses = {gpu}});
            reentry->Last = first;
            SubmissionStatusOverride::NextSubmit = VK_ERROR_OUT_OF_HOST_MEMORY;
            NTest::ExpectError(NCommon::EError::INVALID_STATE,
                               [&] { static_cast<void>(submissions.SubmitGraphics({.ResourceUses = {gpu}})); });
            const auto second = submissions.SubmitGraphics({.ResourceUses = {gpu}});
            reentry->Last = second;
            EXPECT_EQ(second.Value, first.Value + 1);
            gpu.reset();
            SubmissionStatusOverride::NextIdle = VK_ERROR_OUT_OF_HOST_MEMORY;
            NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(submissions.Shutdown()); });
            EXPECT_FALSE(lifetime.expired());
            NTest::ExpectError(NCommon::EError::INVALID_STATE,
                               [&] { static_cast<void>(submissions.SubmitGraphics({})); });
            EXPECT_EQ(submissions.Shutdown(), NVulkan::EVulkanSubmissionShutdown::DRAINED);
            EXPECT_TRUE(lifetime.expired());
            EXPECT_TRUE(reentry->Completed);
        }
        {
            NVulkan::VulkanSubmissionManager submissions{runtime.Device};
            SubmissionStatusOverride override;
            auto gpu = std::make_shared<const int>(1);
            const std::weak_ptr<const int> lifetime = gpu;
            // Status injection uses no native commands or GPU allocation on a healthy device.
            SubmissionStatusOverride::NextSubmit = VK_ERROR_DEVICE_LOST;
            NTest::ExpectError(NCommon::EError::INVALID_STATE,
                               [&] { static_cast<void>(submissions.SubmitGraphics({.ResourceUses = {gpu}})); });
            gpu.reset();
            EXPECT_TRUE(lifetime.expired());
            NTest::ExpectError(NCommon::EError::INVALID_STATE,
                               [&] { static_cast<void>(submissions.IsCompleted({.Value = 1})); });
            EXPECT_EQ(submissions.Shutdown(), NVulkan::EVulkanSubmissionShutdown::DEVICE_LOST);
        }
        EXPECT_EQ(runtime.ValidationErrors->load(), 0U);
    } catch (const NCommon::Exception& exception) {
        HandleRequiredEnvironment(exception);
        throw;
    }
}

TEST(VulkanSubmissionManager, RetainsAllocationUntilLastActualConsumer) {
    RunSpawnedSmoke("VulkanSubmissionManagerChild.DISABLED_RetainsAllocationUntilLastActualConsumer");
}

TEST(VulkanSubmissionManager, RollbackShutdownRetryAndDeviceLoss) {
    RunSpawnedSmoke("VulkanSubmissionManagerChild.DISABLED_RollbackShutdownRetryAndDeviceLoss");
}

} // namespace
