#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <semaphore>

#include <engine/engine.h>
#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;

class OverlapRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    OverlapRuntime() {
        for (std::size_t index = 0; index < GpuSignals.size(); ++index) {
            GpuCompletions[index] = GpuSignals[index].get_future().share();
            Submitted[index] = SubmittedSignals[index].get_future();
        }
        RenderStarted = RenderStartedSignal.get_future();
        NextUpdateStarted = NextUpdateStartedSignal.get_future();
    }

    void Update(const NEngine::NRuntime::FrameContext& frame) override {
        if (frame.GetFrame().GetFrameIndex() == 2) {
            NextUpdateOrder.store(++TraceSequence, std::memory_order_release);
            NextUpdateStartedSignal.set_value();
            NextUpdateRelease.acquire();
        }
    }

    void Extract(const NEngine::NRuntime::FrameContext& frame) override {
        if (frame.GetFrame().GetFrameIndex() == 1) {
            ExtractOrder.store(++TraceSequence, std::memory_order_release);
        }
    }

    void Draw(const NEngine::NRuntime::FrameContext& frame) override {
        const auto index = static_cast<std::size_t>(frame.GetFrame().GetFrameIndex());
        if (index == 1) {
            RenderOrder.store(++TraceSequence, std::memory_order_release);
            RenderStartedSignal.set_value();
            RenderRelease.acquire();
        }
        frame.RegisterGpuCompletion(GpuCompletions[index]);
        SubmittedSignals[index].set_value();
    }

    void ReleaseRender() noexcept {
        if (!RenderReleased.exchange(true)) {
            RenderRelease.release();
        }
    }

    void ReleaseNextUpdate() noexcept {
        if (!NextUpdateReleased.exchange(true)) {
            NextUpdateRelease.release();
        }
    }

    std::array<std::promise<void>, 3> GpuSignals;
    std::array<std::shared_future<void>, 3> GpuCompletions;
    std::array<std::promise<void>, 3> SubmittedSignals;
    std::array<std::future<void>, 3> Submitted;
    std::promise<void> RenderStartedSignal;
    std::future<void> RenderStarted;
    std::binary_semaphore RenderRelease{0};
    std::atomic_bool RenderReleased{false};
    std::promise<void> NextUpdateStartedSignal;
    std::future<void> NextUpdateStarted;
    std::binary_semaphore NextUpdateRelease{0};
    std::atomic_bool NextUpdateReleased{false};
    std::atomic<int> TraceSequence{0};
    std::atomic<int> ExtractOrder{0};
    std::atomic<int> RenderOrder{0};
    std::atomic<int> NextUpdateOrder{0};
};

struct ReleaseOverlap {
    OverlapRuntime& Runtime;
    ~ReleaseOverlap() {
        Runtime.ReleaseRender();
        Runtime.ReleaseNextUpdate();
        for (auto& signal: Runtime.GpuSignals) {
            try {
                signal.set_value();
            } catch (...) {
            }
        }
    }
};

TEST(EngineFrameOverlap, UpdateRunsDuringRenderWithPreviousGpuInFlight) {
    auto runtime = std::make_unique<OverlapRuntime>();
    auto& observed = *runtime;
    auto engine =
            NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = 3, .WorkerCount = 3}, std::move(runtime));
    const ReleaseOverlap release{observed};
    engine->Start();

    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(observed.Submitted[0].wait_for(2s), std::future_status::ready);
    ASSERT_EQ(observed.GpuCompletions[0].wait_for(0s), std::future_status::timeout);

    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(observed.RenderStarted.wait_for(2s), std::future_status::ready);
    ASSERT_GT(observed.ExtractOrder.load(std::memory_order_acquire), 0);

    ASSERT_TRUE(engine->Update());
    ASSERT_EQ(observed.NextUpdateStarted.wait_for(2s), std::future_status::ready);
    const int extractOrder = observed.ExtractOrder.load(std::memory_order_acquire);
    const int renderOrder = observed.RenderOrder.load(std::memory_order_acquire);
    const int updateOrder = observed.NextUpdateOrder.load(std::memory_order_acquire);
    EXPECT_LT(extractOrder, renderOrder);
    EXPECT_LT(renderOrder, updateOrder);
    std::cout << "Frame pipeline trace: " << extractOrder << " Extract(1), " << renderOrder << " Render(1), "
              << updateOrder << " Update(2); GPU(0) pending\n";
    ASSERT_EQ(observed.GpuCompletions[0].wait_for(0s), std::future_status::timeout);
    ASSERT_EQ(observed.Submitted[1].wait_for(0s), std::future_status::timeout);
    EXPECT_FALSE(engine->TryBeginFrame());

    observed.ReleaseNextUpdate();
    ASSERT_TRUE(engine->Draw());
    EXPECT_FALSE(engine->TryBeginFrame());
    observed.ReleaseRender();
    ASSERT_EQ(observed.Submitted[1].wait_for(2s), std::future_status::ready);
    ASSERT_EQ(observed.Submitted[2].wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(engine->TryBeginFrame());

    for (auto& signal: observed.GpuSignals) {
        signal.set_value();
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::optional<NEngine::FrameKey> next;
    while (!next && std::chrono::steady_clock::now() < deadline) {
        next = engine->TryBeginFrame();
        if (!next) {
            const auto version = engine->GetProgressVersion();
            engine->WaitForProgress(version, 1ms);
        }
    }
    ASSERT_TRUE(next);
    if (next) {
        EXPECT_TRUE(engine->AbortFrame(*next));
    }
    engine->Stop();
    EXPECT_FALSE(engine->GetLastError());
}

} // namespace
