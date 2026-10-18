#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <map>
#include <memory>
#include <mutex>
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

#include <engine/runtime/frame_runtime.h>
#include <graphics/vulkan/device.h>
#include <graphics/vulkan/frame_context.h>
#include <graphics/vulkan/glfw_surface.h>
#include <graphics/vulkan/instance.h>
#include <graphics/vulkan/physical_device.h>
#include <graphics/vulkan/submission_manager.h>
#include <gtest/gtest.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>
#include <window/window.h>
#include <window/window_config.h>
#include <window/window_runtime.h>
#include <window/window_size.h>
#include <window/window_type.h>

namespace {

bool IsEnvironmentFailure(const NCommon::Exception& exception) {
    const std::string& message = exception.GetMessage();

    return message.find("VK_ERROR_INCOMPATIBLE_DRIVER") != std::string::npos ||
           message.find("Failed to create GLFW window") != std::string::npos ||
           message.find("Failed to initialize GLFW") != std::string::npos ||
           message.find("Failed to get GLFW Vulkan instance extensions") != std::string::npos;
}

NWindow::WindowConfig MakeWindowConfig() {
    NWindow::WindowConfig windowConfig{NWindow::EWindowType::GLFW};
    windowConfig.Title = "GraphicsEngine Vulkan Frame Context Test";
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
    FAIL() << "Vulkan frame context smoke timed out";
}

struct FenceDeleter {
    VkDevice Device = VK_NULL_HANDLE;

    void operator()(VkFence fence) const noexcept {
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(Device, fence, nullptr);
        }
    }
};

using UniqueFence = std::unique_ptr<std::remove_pointer_t<VkFence>, FenceDeleter>;

UniqueFence CreateFence(VkDevice device) {
    const VkFenceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };

    VkFence fence = VK_NULL_HANDLE;
    EXPECT_EQ(vkCreateFence(device, &createInfo, nullptr, &fence), VK_SUCCESS);

    return UniqueFence{fence, FenceDeleter{device}};
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

void RecordEmptyCommandBuffer(VkCommandBuffer commandBuffer) {
    const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    ASSERT_EQ(vkBeginCommandBuffer(commandBuffer, &beginInfo), VK_SUCCESS);
    ASSERT_EQ(vkEndCommandBuffer(commandBuffer), VK_SUCCESS);
}

void SubmitCommandBuffer(const NVulkan::VulkanDevice& device, VkCommandBuffer commandBuffer, VkFence fence) {
    const VkCommandBufferSubmitInfo commandBufferInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = commandBuffer,
    };
    const VkSubmitInfo2 submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &commandBufferInfo,
    };

    const NVulkan::VulkanLockedQueue graphicsQueue = device.LockGraphicsQueue();
    ASSERT_EQ(vkQueueSubmit2(graphicsQueue.GetHandle(), 1, &submitInfo, fence), VK_SUCCESS);
}

struct VulkanRuntime {
    std::atomic<int> ValidationErrors = 0;
    NWindow::WindowRuntime WindowRuntime;
    NWindow::Window Window{MakeWindowConfig()};
    NVulkan::VulkanInstance Instance{MakeInstanceConfig()};
    NVulkan::NGlfw::VulkanSurface Surface{Instance, Window};
    NVulkan::VulkanPhysicalDeviceSelection PhysicalDevice =
            NVulkan::SelectVulkanPhysicalDevice(Instance, Surface.GetHandle());
    NVulkan::VulkanDevice Device{PhysicalDevice};

    [[nodiscard]] NVulkan::VulkanInstanceConfig MakeInstanceConfig() {
        NVulkan::VulkanInstanceConfig instanceConfig;
        instanceConfig.ApplicationName = "GraphicsEngineVulkanFrameContextTest";
        instanceConfig.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
        instanceConfig.ValidationMode = NVulkan::EValidationMode::Required;
        instanceConfig.EnableDebugUtils = true;
        instanceConfig.DebugMessageHandler = [this](const NVulkan::VulkanDebugMessage& message) {
            if ((message.Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                ++ValidationErrors;
                ADD_FAILURE() << message.Message;
            }
        };
        return instanceConfig;
    }
};

class TimelineFrameRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    TimelineFrameRuntime(const NVulkan::VulkanDevice& device, std::size_t slots)
        : m_device(device)
        , m_submissions(device)
        , m_ring(device, slots)
        , m_commands(slots, VK_NULL_HANDLE)
        , m_events([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                {
                    const std::lock_guard lock{m_mutex};
                    for (auto it = m_pending.begin(); it != m_pending.end();) {
                        try {
                            if (!m_submissions.IsCompleted(it->second.Completion)) {
                                ++it;
                                continue;
                            }
                            it->second.Signal.set_value();
                            ++Completed;
                        } catch (...) {
                            it->second.Signal.set_exception(std::current_exception());
                        }
                        it = m_pending.erase(it);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }) {
    }

    void Update(const NEngine::NRuntime::FrameContext&) override {
    }
    void Draw(const NEngine::NRuntime::FrameContext& frame) override {
        const std::lock_guard lock{m_mutex};
        const auto handle = frame.GetFrame();
        const NVulkan::VulkanFrameContextAcquire acquire{.FrameIndex = handle.GetFrameIndex(),
                                                         .FrameSlotIndex = handle.GetSlotIndex()};
        auto* context = m_ring.TryAcquire(acquire, [&](std::uint64_t value) {
            return m_submissions.IsCompleted({.Value = value});
        });
        if (context == nullptr) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Engine recycled a GPU frame slot too early");
        }
        VkCommandBuffer& command = m_commands[handle.GetSlotIndex()];
        if (command == VK_NULL_HANDLE) {
            command = AllocateCommandBuffer(m_device.GetHandle(), context->GetCommandPool());
        }
        RecordEmptyCommandBuffer(command);
        const VkCommandBufferSubmitInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                             .commandBuffer = command};
        auto [pending, inserted] = m_pending.try_emplace(handle.GetFrameIndex());
        ASSERT_TRUE(inserted);
        try {
            frame.RegisterGpuCompletion(pending->second.Signal.get_future().share());
            pending->second.Completion = m_submissions.SubmitGraphics({.CommandBuffers = {info}});
        } catch (...) {
            pending->second.Signal.set_exception(std::current_exception());
            m_pending.erase(pending);
            throw;
        }
        m_ring.MarkSubmitted(acquire, pending->second.Completion.Value);
    }

    std::atomic<int> Completed = 0;

private:
    const NVulkan::VulkanDevice& m_device;
    NVulkan::VulkanSubmissionManager m_submissions;
    NVulkan::VulkanFrameContextRing m_ring;
    std::vector<VkCommandBuffer> m_commands;
    std::mutex m_mutex;
    struct PendingCompletion {
        NVulkan::VulkanQueueCompletion Completion;
        std::promise<void> Signal;
    };
    std::map<std::uint64_t, PendingCompletion> m_pending;
    std::jthread m_events;
};

TEST(VulkanFrameContextChild, DISABLED_RecyclesEngineSlotsAfterTimelineEvents) {
    try {
        VulkanRuntime runtime;
        for (const std::size_t slots: {1U, 3U}) {
            auto frameRuntime = std::make_unique<TimelineFrameRuntime>(runtime.Device, slots);
            auto& gpu = *frameRuntime;
            auto engine = NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = slots, .WorkerCount = 1},
                                                                   std::move(frameRuntime));
            engine->Start();
            int submitted = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (submitted < 12 && std::chrono::steady_clock::now() < deadline) {
                if (engine->Update()) {
                    ASSERT_TRUE(engine->Draw());
                    ++submitted;
                } else {
                    std::this_thread::yield();
                }
            }
            EXPECT_EQ(submitted, 12);
            engine->Stop();
            EXPECT_EQ(gpu.Completed.load(), 12);
            EXPECT_FALSE(engine->GetLastError().has_value());
        }
        EXPECT_EQ(runtime.ValidationErrors.load(), 0);
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }
        throw;
    }
}

TEST(VulkanFrameContext, RecyclesEngineSlotsAfterTimelineEvents) {
    RunSpawnedSmoke("VulkanFrameContextChild.DISABLED_RecyclesEngineSlotsAfterTimelineEvents");
}

TEST(VulkanFrameContextChild, DISABLED_UsesOneContextPerFrameSlotAndBlocksReuseUntilCompletion) {
    try {
        VulkanRuntime runtime;
        NVulkan::VulkanFrameContextRing ring{runtime.Device, 2};

        EXPECT_EQ(ring.GetSize(), 2U);

        NVulkan::VulkanFrameContext* first = ring.TryAcquire({.FrameIndex = 0, .FrameSlotIndex = 0}, nullptr);
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first->GetFrameIndex(), 0U);
        EXPECT_EQ(first->GetFrameSlotIndex(), 0U);
        EXPECT_NE(first->GetCommandPool(), VK_NULL_HANDLE);

        NVulkan::VulkanFrameContext* second = ring.TryAcquire({.FrameIndex = 1, .FrameSlotIndex = 1}, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_EQ(second->GetFrameIndex(), 1U);
        EXPECT_EQ(second->GetFrameSlotIndex(), 1U);
        EXPECT_NE(second->GetCommandPool(), VK_NULL_HANDLE);
        EXPECT_NE(second->GetCommandPool(), first->GetCommandPool());

        ring.ReleaseAcquired({.FrameIndex = 1, .FrameSlotIndex = 1});
        NVulkan::VulkanFrameContext* reacquired = ring.TryAcquire({.FrameIndex = 3, .FrameSlotIndex = 1}, nullptr);
        ASSERT_NE(reacquired, nullptr);
        EXPECT_EQ(reacquired, second);
        EXPECT_EQ(reacquired->GetFrameIndex(), 3U);
        ring.ReleaseAcquired({.FrameIndex = 3, .FrameSlotIndex = 1});

        EXPECT_EQ(ring.TryAcquire({.FrameIndex = 2, .FrameSlotIndex = 0}, [](std::uint64_t) { return true; }), nullptr);

        ring.MarkSubmitted({.FrameIndex = 0, .FrameSlotIndex = 0}, 7);

        EXPECT_EQ(ring.TryAcquire({.FrameIndex = 2, .FrameSlotIndex = 0}, [](std::uint64_t) { return false; }),
                  nullptr);

        NVulkan::VulkanFrameContext* reused =
                ring.TryAcquire({.FrameIndex = 2, .FrameSlotIndex = 0}, [](std::uint64_t value) { return value == 7; });
        ASSERT_NE(reused, nullptr);
        EXPECT_EQ(reused, first);
        EXPECT_EQ(reused->GetFrameIndex(), 2U);
        EXPECT_FALSE(reused->GetCompletionValue().has_value());
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }

        throw;
    }
}

TEST(VulkanFrameContextChild, DISABLED_SupportsSingleFrameRingAndShutdownInFlight) {
    try {
        VulkanRuntime runtime;
        const UniqueFence fence = CreateFence(runtime.Device.GetHandle());
        NVulkan::VulkanFrameContextRing ring{runtime.Device, 1};

        NVulkan::VulkanFrameContext* context = ring.TryAcquire({.FrameIndex = 0, .FrameSlotIndex = 0}, nullptr);
        ASSERT_NE(context, nullptr);
        VkCommandBuffer commandBuffer = AllocateCommandBuffer(runtime.Device.GetHandle(), context->GetCommandPool());
        RecordEmptyCommandBuffer(commandBuffer);
        SubmitCommandBuffer(runtime.Device, commandBuffer, fence.get());
        ring.MarkSubmitted({.FrameIndex = 0, .FrameSlotIndex = 0}, 1);
    } catch (const NCommon::Exception& exception) {
        if (IsEnvironmentFailure(exception)) {
            GTEST_SKIP() << exception.GetMessage();
        }

        throw;
    }
}

TEST(VulkanFrameContext, UsesOneContextPerFrameSlotAndBlocksReuseUntilCompletion) {
    RunSpawnedSmoke("VulkanFrameContextChild.DISABLED_UsesOneContextPerFrameSlotAndBlocksReuseUntilCompletion");
}

TEST(VulkanFrameContext, SupportsSingleFrameRingAndShutdownInFlight) {
    RunSpawnedSmoke("VulkanFrameContextChild.DISABLED_SupportsSingleFrameRingAndShutdownInFlight");
}

} // namespace
