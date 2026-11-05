#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <stdexcept>

#include <engine/controller/frame_scheduler.h>
#include <engine/engine.h>
#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {

using namespace std::chrono_literals;

struct HeldUpdate {
    NEngine::NController::FrameHandle Frame;
    std::shared_ptr<NEngine::NRuntime::UpdateCompletion> Completion;
};

class CheckpointRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    CheckpointRuntime() {
        for (std::size_t index = 0; index < Updates.size(); ++index) {
            Scheduled[index] = Updates[index].get_future().share();
            GpuCompletions[index] = GpuSignals[index].get_future().share();
            Drawn[index] = DrawSignals[index].get_future().share();
        }
    }

    void Update(const NEngine::NRuntime::FrameContext&) override {
        ADD_FAILURE() << "Semantic async update was bypassed";
    }

    void ScheduleUpdate(const NEngine::NRuntime::FrameContext& frame,
                        NCommon::TaskSystem&,
                        std::shared_ptr<NEngine::NRuntime::UpdateCompletion> completion) override {
        struct FrameProbe {
            explicit FrameProbe(std::atomic<int>& retired)
                : Retired(retired) {
            }
            ~FrameProbe() {
                ++Retired;
            }
            std::atomic<int>& Retired;
        };
        static_cast<void>(frame.GetStorage().Emplace<FrameProbe>(Retired));
        completion->BindCancellation([weak = std::weak_ptr{completion}] {
            if (const auto pending = weak.lock()) {
                pending->Finish({.Status = NEngine::NRuntime::EUpdateStatus::CANCELLED});
            }
        });
        Updates.at(static_cast<std::size_t>(UpdateCount.fetch_add(1))).set_value({frame.GetFrame(), completion});
    }

    void Extract(const NEngine::NRuntime::FrameContext&) override {
        ++ExtractCount;
    }

    void Draw(const NEngine::NRuntime::FrameContext& frame) override {
        const auto index = static_cast<std::size_t>(frame.GetFrame().GetFrameIndex());
        frame.RegisterGpuCompletion(GpuCompletions.at(index));
        ++DrawCount;
        DrawSignals.at(index).set_value();
    }

    void Drain() noexcept override {
        for (auto& signal: GpuSignals) {
            try {
                signal.set_value();
            } catch (const std::future_error&) {
            }
        }
    }

    std::array<std::promise<HeldUpdate>, 12> Updates;
    std::array<std::shared_future<HeldUpdate>, 12> Scheduled;
    std::array<std::promise<void>, 12> GpuSignals;
    std::array<std::shared_future<void>, 12> GpuCompletions;
    std::array<std::promise<void>, 12> DrawSignals;
    std::array<std::shared_future<void>, 12> Drawn;
    std::atomic<int> UpdateCount = 0;
    std::atomic<int> ExtractCount = 0;
    std::atomic<int> DrawCount = 0;
    std::atomic<int> Retired = 0;
};

template<typename Predicate>
bool Eventually(NEngine::Engine& engine, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto version = engine.GetProgressVersion();
        if (predicate()) {
            return true;
        }
        engine.WaitForProgress(version, 1ms);
    }
    return false;
}

TEST(EngineLegacyCheckpoint, DrawIntentPreservesBackpressureAndSlotLifetime) {
    for (std::size_t workers: {1U, 2U, 8U}) {
        for (std::size_t slots: {1U, 2U, 3U}) {
            SCOPED_TRACE(::testing::Message() << "workers=" << workers << " slots=" << slots);
            auto runtime = std::make_unique<CheckpointRuntime>();
            auto& observed = *runtime;
            auto engine = NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = slots, .WorkerCount = workers},
                                                                   std::move(runtime));
            engine->Start();
            for (std::size_t frame = 0; frame < 12; ++frame) {
                ASSERT_TRUE(Eventually(*engine, [&] { return engine->Update(); }));
                ASSERT_EQ(observed.Scheduled[frame].wait_for(2s), std::future_status::ready);
                const HeldUpdate& update = observed.Scheduled[frame].get();
                EXPECT_EQ(update.Frame.GetFrameIndex(), frame);
                EXPECT_EQ(update.Frame.GetSlotIndex(), frame % slots);
                ASSERT_TRUE(engine->Draw());
                EXPECT_NO_THROW(EXPECT_FALSE(engine->Update()));
                EXPECT_FALSE(engine->CanAcceptUpdate());
                EXPECT_EQ(observed.UpdateCount.load(), frame + 1);
                EXPECT_EQ(observed.ExtractCount.load(), frame);
                EXPECT_EQ(observed.DrawCount.load(), frame);
                auto independent = engine->RunAsync([] { return 17; });
                ASSERT_TRUE(Eventually(*engine, [&] { return independent.IsReady(); }));
                EXPECT_EQ(independent.TryTake()->value(), 17);
                update.Completion->Finish();
                ASSERT_EQ(observed.Drawn[frame].wait_for(2s), std::future_status::ready);
                EXPECT_EQ(observed.ExtractCount.load(), frame + 1);
                EXPECT_EQ(observed.DrawCount.load(), frame + 1);
                if ((frame + 1) % slots == 0) {
                    EXPECT_NO_THROW(EXPECT_FALSE(engine->Update()));
                    EXPECT_EQ(observed.Retired.load(), frame + 1 - slots);
                    for (std::size_t index = frame + 1 - slots; index <= frame; ++index) {
                        observed.GpuSignals[index].set_value();
                    }
                }
            }
            engine->Stop();
            EXPECT_EQ(observed.UpdateCount.load(), 12);
            EXPECT_EQ(observed.ExtractCount.load(), 12);
            EXPECT_EQ(observed.DrawCount.load(), 12);
            EXPECT_EQ(observed.Retired.load(), 12);
            EXPECT_FALSE(engine->GetLastError());
        }
    }
}

TEST(EngineLegacyCheckpoint, RejectsMissingAndDuplicateCheckpoints) {
    for (std::size_t slots: {1U, 2U, 3U}) {
        auto runtime = std::make_unique<CheckpointRuntime>();
        auto& observed = *runtime;
        auto engine = NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = slots, .WorkerCount = 1},
                                                               std::move(runtime));
        engine->Start();
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)engine->Draw(); });
        ASSERT_TRUE(engine->Update());
        ASSERT_EQ(observed.Scheduled[0].wait_for(2s), std::future_status::ready);
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)engine->Update(); });
        ASSERT_TRUE(engine->Draw());
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)engine->Draw(); });
        EXPECT_NO_THROW(EXPECT_FALSE(engine->Update()));
        observed.Scheduled[0].get().Completion->Finish();
        engine->Stop();
        EXPECT_EQ(observed.UpdateCount.load(), 1);
        EXPECT_EQ(observed.DrawCount.load(), 1);
        EXPECT_FALSE(engine->GetLastError());
    }
}

TEST(EngineLegacyCheckpoint, FailedOrCancelledUpdateCannotAdmitOrAffectRestart) {
    for (const auto status: {NEngine::NRuntime::EUpdateStatus::FAILED, NEngine::NRuntime::EUpdateStatus::CANCELLED}) {
        auto runtime = std::make_unique<CheckpointRuntime>();
        auto& observed = *runtime;
        auto engine =
                NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = 1, .WorkerCount = 1}, std::move(runtime));
        engine->Start();
        const auto old = engine->TryBeginFrame();
        ASSERT_TRUE(old);
        ASSERT_TRUE(engine->CompleteUserUpdate(*old));
        ASSERT_EQ(observed.Scheduled[0].wait_for(2s), std::future_status::ready);
        ASSERT_TRUE(engine->Draw());
        const auto completion = observed.Scheduled[0].get().Completion;
        completion->Finish({.Status = status, .Error = std::make_exception_ptr(std::runtime_error("Update rejected"))});
        EXPECT_EQ(engine->GetState(), NEngine::EEngineState::STOPPING);
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)engine->Update(); });
        EXPECT_FALSE(engine->CompleteUserUpdate(*old));
        engine->Stop();
        EXPECT_EQ(observed.ExtractCount.load(), 0);
        EXPECT_EQ(observed.DrawCount.load(), 0);
        engine->Start();
        const auto next = engine->TryBeginFrame();
        ASSERT_TRUE(next);
        EXPECT_NE(next->GetRunGeneration(), old->GetRunGeneration());
        completion->Finish();
        EXPECT_FALSE(engine->CompleteUserUpdate(*old));
        EXPECT_FALSE(engine->IsUpdateComplete(*next));
        ASSERT_TRUE(engine->CompleteUserUpdate(*next));
        ASSERT_EQ(observed.Scheduled[1].wait_for(2s), std::future_status::ready);
        observed.Scheduled[1].get().Completion->Finish();
        EXPECT_TRUE(engine->Draw());
        engine->Stop();
        EXPECT_EQ(observed.DrawCount.load(), 1);
        EXPECT_EQ(observed.Retired.load(), 2);
        EXPECT_FALSE(engine->GetLastError());
    }
}

TEST(EngineLegacyCheckpoint, StopCancelsHeldUpdateWithoutDrawing) {
    auto runtime = std::make_unique<CheckpointRuntime>();
    auto& observed = *runtime;
    auto engine =
            NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = 1, .WorkerCount = 1}, std::move(runtime));
    engine->Start();
    ASSERT_TRUE(engine->Update());
    ASSERT_EQ(observed.Scheduled[0].wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(engine->Draw());
    engine->Stop();
    observed.Scheduled[0].get().Completion->Finish();
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)engine->Update(); });
    EXPECT_EQ(observed.ExtractCount.load(), 0);
    EXPECT_EQ(observed.DrawCount.load(), 0);
    EXPECT_EQ(observed.Retired.load(), 1);
    EXPECT_EQ(engine->GetState(), NEngine::EEngineState::STOPPED);
}

} // namespace
