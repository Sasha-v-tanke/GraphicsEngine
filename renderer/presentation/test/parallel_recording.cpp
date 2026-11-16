#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <vector>
#include <vulkan.h>

#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <renderer/recording_plan.h>
#include <samples/sample_6_textured/scenario.h>

namespace {

// Real-driver dispatch probe: observe command order and actual raster samples, and inject
// one secondary-end failure. Hooks are installed after Volk loads device functions, and
// restored before any referenced state dies. Only metadata uses the probe mutex.
class RecordingProbe final {
public:
    explicit RecordingProbe(std::size_t draws, bool reorder = false, bool fail = false)
        : Draws(draws)
        , Reorder(reorder)
        , Fail(fail) {
        Active = this;
        OriginalAllocate = vkAllocateCommandBuffers;
        OriginalBegin = vkBeginCommandBuffer;
        OriginalEnd = vkEndCommandBuffer;
        OriginalPush = vkCmdPushConstants;
        OriginalDraw = vkCmdDrawIndexed;
        OriginalExecute = vkCmdExecuteCommands;
        OriginalSubmit = vkQueueSubmit2;
        vkAllocateCommandBuffers = Allocate;
        vkBeginCommandBuffer = Begin;
        vkEndCommandBuffer = End;
        vkCmdPushConstants = Push;
        vkCmdDrawIndexed = Draw;
        vkCmdExecuteCommands = Execute;
        vkQueueSubmit2 = Submit;
    }
    ~RecordingProbe() {
        // The test destroys presentation first, so GPU consumers have drained.
        if (Pool != VK_NULL_HANDLE) {
            // Destroyed explicitly by Finish before presentation's native device disappears.
            std::terminate();
        }
        vkAllocateCommandBuffers = OriginalAllocate;
        vkBeginCommandBuffer = OriginalBegin;
        vkEndCommandBuffer = OriginalEnd;
        vkCmdPushConstants = OriginalPush;
        vkCmdDrawIndexed = OriginalDraw;
        vkCmdExecuteCommands = OriginalExecute;
        vkQueueSubmit2 = OriginalSubmit;
        Active = nullptr;
    }
    void Finish() {
        if (Pool != VK_NULL_HANDLE) {
            ASSERT_EQ(vkDeviceWaitIdle(Device), VK_SUCCESS);
            if (!Fail && Submits > 0) {
                Samples.resize(Draws);
                EXPECT_EQ(vkGetQueryPoolResults(Device,
                                                Pool,
                                                0,
                                                static_cast<std::uint32_t>(Draws),
                                                Samples.size() * sizeof(std::uint64_t),
                                                Samples.data(),
                                                sizeof(std::uint64_t),
                                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                          VK_SUCCESS);
            }
            vkDestroyQueryPool(Device, Pool, nullptr);
            Pool = VK_NULL_HANDLE;
        }
    }
    std::vector<std::uint64_t> Samples;
    std::vector<std::size_t> Execution;
    std::vector<std::size_t> FinishedChunks;
    std::atomic_size_t SecondaryAllocations{0};
    std::atomic_size_t SecondaryBegins{0};
    std::atomic_size_t ActiveRecordings{0};
    std::atomic_size_t Submits{0};
    std::atomic_bool ProbeFailure{false};

private:
    static VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice device,
                                                   const VkCommandBufferAllocateInfo* info,
                                                   VkCommandBuffer* buffers) {
        if (info->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY && Active->Pool == VK_NULL_HANDLE) {
            Active->Device = device;
            VkQueryPoolCreateInfo query{};
            query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query.queryType = VK_QUERY_TYPE_OCCLUSION;
            query.queryCount = static_cast<std::uint32_t>(Active->Draws);
            if (vkCreateQueryPool(device, &query, nullptr, &Active->Pool) != VK_SUCCESS) {
                Active->ProbeFailure = true;
            }
        }
        if (info->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
            Active->SecondaryAllocations += info->commandBufferCount;
        }
        return Active->OriginalAllocate(device, info, buffers);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Begin(VkCommandBuffer command, const VkCommandBufferBeginInfo* info) {
        const VkResult result = Active->OriginalBegin(command, info);
        if (result != VK_SUCCESS) {
            return result;
        }
        const bool secondary = (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) != 0;
        {
            const std::lock_guard lock{Active->Mutex};
            Active->Commands[command].clear();
            Active->Secondary[command] = secondary;
            if (!secondary) {
                Active->Execution.clear();
            }
        }
        if (secondary) {
            ++Active->SecondaryBegins;
            ++Active->ActiveRecordings;
        } else if (Active->Pool != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(command, Active->Pool, 0, static_cast<std::uint32_t>(Active->Draws));
        }
        return result;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL End(VkCommandBuffer command) {
        bool secondary = false;
        std::size_t first = 0;
        {
            const std::lock_guard lock{Active->Mutex};
            secondary = Active->Secondary.at(command);
            if (secondary) {
                first = Active->Commands.at(command).front();
                Active->FinishedChunks.push_back(first);
                if (first != 0) {
                    Active->LaterChunk = true;
                    Active->Progress.notify_all();
                }
            }
        }
        if (secondary) {
            --Active->ActiveRecordings;
            if (Active->Fail && !Active->Injected.exchange(true)) {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        return Active->OriginalEnd(command);
    }
    static VKAPI_ATTR void VKAPI_CALL Push(VkCommandBuffer command,
                                           VkPipelineLayout layout,
                                           VkShaderStageFlags stages,
                                           std::uint32_t offset,
                                           std::uint32_t size,
                                           const void* values) {
        DrawId = static_cast<std::size_t>(std::lround(static_cast<const float*>(values)[14] * 4096.0F));
        if (Active->Reorder) {
            std::unique_lock lock{Active->Mutex};
            if (DrawId == 0) {
                if (!Active->Progress.wait_for(lock, std::chrono::seconds{3}, [] { return Active->LaterChunk; })) {
                    Active->ProbeFailure = true;
                }
            }
        }
        Active->OriginalPush(command, layout, stages, offset, size, values);
    }
    static VKAPI_ATTR void VKAPI_CALL Draw(VkCommandBuffer command,
                                           std::uint32_t count,
                                           std::uint32_t instances,
                                           std::uint32_t first,
                                           std::int32_t offset,
                                           std::uint32_t instance) {
        {
            const std::lock_guard lock{Active->Mutex};
            Active->Commands[command].push_back(DrawId);
            if (!Active->Secondary.at(command)) {
                Active->Execution.push_back(DrawId);
            }
        }
        if (Active->Pool != VK_NULL_HANDLE && DrawId < Active->Draws) {
            vkCmdBeginQuery(command, Active->Pool, static_cast<std::uint32_t>(DrawId), 0);
            Active->OriginalDraw(command, count, instances, first, offset, instance);
            vkCmdEndQuery(command, Active->Pool, static_cast<std::uint32_t>(DrawId));
        } else {
            Active->ProbeFailure = true;
        }
    }
    static VKAPI_ATTR void VKAPI_CALL Execute(VkCommandBuffer primary,
                                              std::uint32_t count,
                                              const VkCommandBuffer* commands) {
        {
            const std::lock_guard lock{Active->Mutex};
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto& draws = Active->Commands.at(commands[index]);
                Active->Execution.insert(Active->Execution.end(), draws.begin(), draws.end());
            }
        }
        Active->OriginalExecute(primary, count, commands);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue,
                                                 std::uint32_t count,
                                                 const VkSubmitInfo2* submits,
                                                 VkFence fence) {
        Active->Submits += count;
        return Active->OriginalSubmit(queue, count, submits, fence);
    }

    inline static RecordingProbe* Active = nullptr;
    inline static thread_local std::size_t DrawId = 0;
    const std::size_t Draws;
    const bool Reorder;
    const bool Fail;
    bool LaterChunk = false;
    std::atomic_bool Injected{false};
    std::mutex Mutex;
    std::condition_variable Progress;
    std::unordered_map<VkCommandBuffer, std::vector<std::size_t>> Commands;
    std::unordered_map<VkCommandBuffer, bool> Secondary;
    VkDevice Device = VK_NULL_HANDLE;
    VkQueryPool Pool = VK_NULL_HANDLE;
    PFN_vkAllocateCommandBuffers OriginalAllocate;
    PFN_vkBeginCommandBuffer OriginalBegin;
    PFN_vkEndCommandBuffer OriginalEnd;
    PFN_vkCmdPushConstants OriginalPush;
    PFN_vkCmdDrawIndexed OriginalDraw;
    PFN_vkCmdExecuteCommands OriginalExecute;
    PFN_vkQueueSubmit2 OriginalSubmit;
};

std::vector<std::uint64_t> RunRecording(NWindow::Window& window,
                                        NCommon::TaskSystem& tasks,
                                        NRenderer::EGraphRecordingMode mode,
                                        std::size_t slots,
                                        std::size_t largeDraws,
                                        std::atomic<int>& errors,
                                        bool fail = false) {
    NRenderer::GraphPresentation presentation{window,
                                              {.MaxActiveFrames = slots,
                                               .RequireValidation = true,
                                               .ValidationErrorHandler =
                                                       [&](std::string_view message) {
                                                           ++errors;
                                                           std::cerr << message << '\n';
                                                       }},
                                              {mode, &tasks}};
    NTextured::Scene scene;
    scene.Prepare(presentation);
    const std::size_t totalDraws = largeDraws + 3;
    RecordingProbe probe{totalDraws,
                         mode == NRenderer::EGraphRecordingMode::PARALLEL && tasks.GetWorkerCount() > 1 &&
                                 largeDraws > 64,
                         fail};
    // Always release query storage while presentation's native device is still alive, including failures.
    struct Finish {
        RecordingProbe& Probe;
        ~Finish() {
            Probe.Finish();
        }
    } finish{probe};
    const std::size_t frames = fail ? 1 : 8;
    for (std::size_t index = 0; index < frames; ++index) {
        const NRenderer::RenderFrameIdentity frame{.ApplicationFrameIndex = index,
                                                   .SimulationIndex = index,
                                                   .FrameSlotIndex = index % slots,
                                                   .Generation = index / slots + 1};
        bool submitted = false;
        for (std::size_t retry = 0; retry < 100000 && !submitted; ++retry) {
            const auto result = presentation.Render(frame, [&](const auto& target) {
                NRenderer::RenderGraphBuilder builder{frame};
                const auto output = builder.ImportSwapchain(target, NRenderer::EResourceUsage::UNDEFINED);
                for (std::size_t passIndex = 0; passIndex < 2; ++passIndex) {
                    NRenderer::GraphicsPassDescription pass;
                    pass.Writes = {{output, NRenderer::EResourceUsage::COLOR_ATTACHMENT_WRITE}};
                    NRenderer::GraphAttachment attachment;
                    attachment.Resource = output;
                    attachment.Load =
                            passIndex == 0 ? NRenderer::EAttachmentLoad::CLEAR : NRenderer::EAttachmentLoad::LOAD;
                    pass.ColorAttachments = {attachment};
                    const std::size_t count = passIndex == 0 ? largeDraws : 3;
                    for (std::size_t draw = 0; draw < count; ++draw) {
                        NRenderer::DrawCommandData data;
                        data.Mesh = scene.Mesh;
                        data.Material = scene.Material;
                        data.MeshVersion = scene.Resources.TryAcquire(scene.Mesh)->GetVersion();
                        data.MaterialVersion = scene.Resources.TryAcquire(scene.Material)->GetVersion();
                        data.ViewProjection(2, 3) =
                                static_cast<float>(draw + (passIndex == 0 ? 0 : largeDraws)) / 4096.0F;
                        pass.DrawCommands.push_back(data);
                    }
                    static_cast<void>(builder.AddGraphicsPass(std::move(pass)));
                }
                return builder.Compile();
            });
            if (fail && result.Failure) {
                EXPECT_FALSE(result.Completion.IsValid());
                EXPECT_EQ(probe.Submits.load(), 0U);
                EXPECT_EQ(probe.ActiveRecordings.load(), 0U);
                EXPECT_TRUE(presentation
                                    .Render(frame,
                                            [](const auto&) -> NRenderer::CompiledGraph {
                                                throw std::runtime_error{
                                                        "failed path must reject before graph production"};
                                            })
                                    .Failure);
                return {};
            }
            if (result.Failure) {
                std::rethrow_exception(result.Failure);
            }
            submitted = result.Completion.IsValid();
            if (!submitted) {
                std::this_thread::yield();
            }
        }
        EXPECT_TRUE(submitted);
        std::vector<std::size_t> expected(totalDraws);
        std::iota(expected.begin(), expected.end(), 0);
        EXPECT_EQ(probe.Execution, expected);
    }
    EXPECT_FALSE(fail) << "Injected native recording failure was not propagated";
    EXPECT_FALSE(probe.ProbeFailure.load());
    EXPECT_EQ(probe.Submits.load(), frames);
    if (mode == NRenderer::EGraphRecordingMode::PARALLEL && largeDraws > 64) {
        EXPECT_GT(probe.SecondaryBegins.load(), 0U);
        EXPECT_LE(probe.SecondaryAllocations.load(), slots * tasks.GetWorkerCount() * tasks.GetWorkerCount() * 2);
        if (tasks.GetWorkerCount() > 1) {
            EXPECT_FALSE(probe.FinishedChunks.empty());
            if (!probe.FinishedChunks.empty()) {
                EXPECT_NE(probe.FinishedChunks.front(), 0U);
            }
        }
    } else {
        EXPECT_EQ(probe.SecondaryBegins.load(), 0U);
    }
    probe.Finish();
    return probe.Samples;
}

TEST(GraphPresentation, ParallelSecondariesPreserveDrawOrderAndRasterSamplesAcrossWorkersAndSlots) {
    std::atomic<int> errors{0};
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{NWindow::WindowConfig{NWindow::EWindowType::GLFW}};
        for (const std::size_t workers: {1U, 3U}) {
            NCommon::TaskSystem tasks{workers};
            EXPECT_FALSE(RunRecording(window, tasks, NRenderer::EGraphRecordingMode::PARALLEL, 1, 32, errors).empty());
            for (const std::size_t slots: {1U, 3U}) {
                const std::size_t draws = workers == 1 ? 193 : 1025;
                const auto serial =
                        RunRecording(window, tasks, NRenderer::EGraphRecordingMode::SERIAL, slots, draws, errors);
                const auto parallel =
                        RunRecording(window, tasks, NRenderer::EGraphRecordingMode::PARALLEL, slots, draws, errors);
                ASSERT_EQ(serial.size(), draws + 3);
                EXPECT_EQ(parallel, serial);
                EXPECT_TRUE(std::ranges::all_of(parallel, [](std::uint64_t samples) { return samples > 0; }));
            }
        }
    } catch (const NCommon::Exception& error) {
        if (error.code() == NCommon::EError::UNSUPPORTED && errors.load() == 0) {
            GTEST_SKIP() << error.GetMessage();
        }
        throw;
    }
    EXPECT_EQ(errors.load(), 0);
}

TEST(GraphPresentation, SecondaryFailureJoinsSiblingsAndFailsClosedBeforeSubmit) {
    std::atomic<int> errors{0};
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{NWindow::WindowConfig{NWindow::EWindowType::GLFW}};
        for (const std::size_t workers: {1U, 3U}) {
            NCommon::TaskSystem tasks{workers};
            EXPECT_TRUE(RunRecording(window, tasks, NRenderer::EGraphRecordingMode::PARALLEL, 3, 193, errors, true)
                                .empty());
        }
    } catch (const NCommon::Exception& error) {
        if (error.code() == NCommon::EError::UNSUPPORTED && errors.load() == 0) {
            GTEST_SKIP() << error.GetMessage();
        }
        throw;
    }
    EXPECT_EQ(errors.load(), 0);
}

} // namespace
