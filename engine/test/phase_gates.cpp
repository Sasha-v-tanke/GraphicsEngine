#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <vector>

#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>
#include <lib/thread/task/frame_origin.h>

namespace {
using namespace std::chrono_literals;

class PhaseRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    void Trace(int phase, const NEngine::NRuntime::FrameContext& frame) {
        std::lock_guard lock{Mutex};
        Events.push_back(static_cast<int>(frame.GetFrame().GetFrameIndex()) * 5 + phase);
    }
    void Update(const NEngine::NRuntime::FrameContext& frame) override {
        Trace(1, frame);
    }
    void ScheduleUpdate(const NEngine::NRuntime::FrameContext& frame,
                        NCommon::TaskSystem& tasks,
                        std::shared_ptr<NEngine::NRuntime::UpdateCompletion> completion) override {
        if (HoldSemantic) {
            Update(frame);
            completion->BindCancellation([weak = std::weak_ptr{completion}] {
                if (const auto owned = weak.lock()) {
                    owned->Finish({.Status = NEngine::NRuntime::EUpdateStatus::CANCELLED});
                }
            });
            Semantic.set_value(std::move(completion));
            return;
        }
        IFrameRuntime::ScheduleUpdate(frame, tasks, std::move(completion));
    }
    [[nodiscard]] bool HasPostUpdate() const noexcept override {
        return true;
    }
    NCommon::TaskHandle SchedulePostUpdate(const NEngine::NRuntime::FrameContext& frame,
                                           NCommon::TaskContext& task) override {
        const NCommon::TaskHandle dependencies[] = {task.GetTask()};
        return task.Spawn(
                [this, frame](NCommon::TaskContext&) {
                    if (HoldPost) {
                        Entered.set_value();
                        Release.acquire();
                    }
                    if (frame.IsStopRequested()) {
                        return;
                    }
                    Trace(2, frame);
                    if (FailPost) {
                        throw std::runtime_error{"post-update failure"};
                    }
                },
                dependencies);
    }
    void FinalizePostUpdate(const NEngine::NRuntime::FrameContext& frame) override {
        if (HoldCommit) {
            CommitEntered.set_value();
            CommitRelease.acquire();
        }
        Trace(3, frame);
    }
    NCommon::TaskHandle ScheduleExtract(const NEngine::NRuntime::FrameContext& frame,
                                        NCommon::TaskContext& task) override {
        const NCommon::TaskHandle dependencies[] = {task.GetTask()};
        const auto first = task.Spawn([this, frame](NCommon::TaskContext&) { Trace(4, frame); }, dependencies);
        const auto second = task.Spawn(
                [this](NCommon::TaskContext&) {
                    if (HoldExtraction) {
                        ExtractionEntered.set_value();
                        ExtractionRelease.acquire();
                    }
                },
                dependencies);
        const NCommon::TaskHandle readers[] = {first, second};
        return task.Spawn([](NCommon::TaskContext&) {}, readers);
    }
    void Draw(const NEngine::NRuntime::FrameContext& frame) override {
        if (HoldDraw && frame.GetFrame().GetFrameIndex() == 0) {
            DrawEntered.set_value();
            DrawRelease.acquire();
        }
        ++Draws;
    }
    std::mutex Mutex;
    std::vector<int> Events;
    std::atomic<int> Draws{0};
    std::promise<void> Entered;
    std::binary_semaphore Release{0};
    bool HoldCommit = false;
    std::promise<void> CommitEntered;
    std::binary_semaphore CommitRelease{0};
    bool HoldExtraction = false;
    std::promise<void> ExtractionEntered;
    std::binary_semaphore ExtractionRelease{0};
    bool HoldSemantic = false;
    bool HoldDraw = false;
    std::promise<std::shared_ptr<NEngine::NRuntime::UpdateCompletion>> Semantic;
    std::promise<void> DrawEntered;
    std::binary_semaphore DrawRelease{0};
    bool HoldPost = false;
    bool FailPost = false;
};

template<typename Predicate>
bool Progress(NEngine::Engine& engine, Predicate predicate) {
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

TEST(EnginePhaseGates, WorkerPostUpdateCommitAndAllExtractionReadersPrecedeNextMutation) {
    for (std::size_t slots: {1U, 2U, 3U}) {
        for (std::size_t workers: {1U, 2U, 8U}) {
            auto runtime = std::make_unique<PhaseRuntime>();
            auto& observed = *runtime;
            auto engine = NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = slots, .WorkerCount = workers},
                                                                   std::move(runtime));
            engine->Start();
            for (int index = 0; index < 12; ++index) {
                std::optional<NEngine::FrameKey> key;
                ASSERT_TRUE(Progress(*engine, [&] {
                    key = engine->TryBeginFrame();
                    return key.has_value();
                }));
                ASSERT_TRUE(engine->ScheduleUserUpdate(*key, [&, index] {
                    EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
                    std::lock_guard lock{observed.Mutex};
                    observed.Events.push_back(index * 5);
                }));
                ASSERT_TRUE(engine->Draw());
                ASSERT_TRUE(Progress(*engine, [&] { return observed.Draws.load() == index + 1; }));
            }
            engine->Stop();
            EXPECT_FALSE(engine->GetLastError());
            ASSERT_EQ(observed.Events.size(), 60);
            for (std::size_t index = 0; index < observed.Events.size(); ++index) {
                EXPECT_EQ(observed.Events[index], index);
            }
        }
    }
}

TEST(EnginePhaseGates, PostUpdateWaitsForSemanticCompletionAfterOrchestratorReturns) {
    auto runtime = std::make_unique<PhaseRuntime>();
    auto& observed = *runtime;
    observed.HoldSemantic = true;
    auto semantic = observed.Semantic.get_future();
    auto engine =
            NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = 3, .WorkerCount = 1}, std::move(runtime));
    engine->Start();
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(semantic.wait_for(2s), std::future_status::ready);
    auto completion = semantic.get();
    EXPECT_FALSE(engine->TryBeginFrame());
    {
        std::lock_guard lock{observed.Mutex};
        EXPECT_EQ(observed.Events, (std::vector<int>{1}));
    }
    completion->Finish();
    ASSERT_TRUE(Progress(*engine, [&] { return observed.Draws.load() == 1; }));
    engine->Stop();
    EXPECT_EQ(observed.Events, (std::vector<int>{1, 2, 3, 4}));
}

TEST(EnginePhaseGates, ImmutableRenderingDoesNotHoldNextWorldUpdate) {
    auto runtime = std::make_unique<PhaseRuntime>();
    auto& observed = *runtime;
    observed.HoldDraw = true;
    auto entered = observed.DrawEntered.get_future();
    auto engine =
            NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = 2, .WorkerCount = 2}, std::move(runtime));
    struct ReleaseGuard {
        PhaseRuntime& Runtime;
        ~ReleaseGuard() {
            Runtime.DrawRelease.release();
        }
    } guard{observed};
    engine->Start();
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
    std::optional<NEngine::FrameKey> next;
    ASSERT_TRUE(Progress(*engine, [&] {
        next = engine->TryBeginFrame();
        return next.has_value();
    }));
    EXPECT_EQ(next->GetFrameSequence(), 1);
    EXPECT_EQ(observed.Draws.load(), 0);
    EXPECT_TRUE(engine->AbortFrame(*next));
    observed.DrawRelease.release();
    engine->Stop();
    EXPECT_FALSE(engine->GetLastError());
}

TEST(EnginePhaseGates, EveryExtractionReaderMustRetireBeforeNextAdmission) {
    auto runtime = std::make_unique<PhaseRuntime>();
    auto& observed = *runtime;
    observed.HoldExtraction = true;
    auto entered = observed.ExtractionEntered.get_future();
    auto engine =
            NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = 2, .WorkerCount = 2}, std::move(runtime));
    struct ReleaseGuard {
        PhaseRuntime& Runtime;
        ~ReleaseGuard() {
            Runtime.ExtractionRelease.release();
        }
    } guard{observed};
    engine->Start();
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(engine->TryBeginFrame());
    EXPECT_EQ(observed.Draws.load(), 0);
    observed.ExtractionRelease.release();
    std::optional<NEngine::FrameKey> next;
    ASSERT_TRUE(Progress(*engine, [&] {
        next = engine->TryBeginFrame();
        return next.has_value();
    }));
    EXPECT_TRUE(engine->AbortFrame(*next));
    engine->Stop();
    EXPECT_FALSE(engine->GetLastError());
}

TEST(EnginePhaseGates, PostFailureCancelsCommitAndExtraction) {
    auto runtime = std::make_unique<PhaseRuntime>();
    auto& observed = *runtime;
    observed.FailPost = true;
    auto engine = NEngine::NRuntime::EngineFactory::Create({.WorkerCount = 1}, std::move(runtime));
    engine->Start();
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_TRUE(Progress(*engine, [&] {
        if (engine->GetState() == NEngine::EEngineState::RUNNING) {
            try {
                (void)engine->TryBeginFrame();
            } catch (...) {
            }
        }
        return engine->GetLastError().has_value();
    }));
    engine->Stop();
    EXPECT_EQ(engine->GetLastError()->Message, "post-update failure");
    EXPECT_EQ(observed.Draws.load(), 0);
    EXPECT_EQ(observed.Events, (std::vector<int>{1, 2}));
}

TEST(EnginePhaseGates, StopDuringCommitSkipsExtractionAfterWritesRetire) {
    auto runtime = std::make_unique<PhaseRuntime>();
    auto& observed = *runtime;
    observed.HoldCommit = true;
    auto entered = observed.CommitEntered.get_future();
    auto engine = NEngine::NRuntime::EngineFactory::Create({.WorkerCount = 1}, std::move(runtime));
    struct ReleaseGuard {
        PhaseRuntime& Runtime;
        ~ReleaseGuard() {
            Runtime.CommitRelease.release();
        }
    } guard{observed};
    engine->Start();
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
    auto stopped = std::async(std::launch::async, [&] { engine->Stop(); });
    EXPECT_TRUE(Progress(*engine, [&] { return engine->GetState() == NEngine::EEngineState::STOPPING; }));
    EXPECT_EQ(stopped.wait_for(5ms), std::future_status::timeout);
    observed.CommitRelease.release();
    ASSERT_EQ(stopped.wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(engine->GetLastError());
    EXPECT_EQ(observed.Events, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(observed.Draws.load(), 0);
}

TEST(EnginePhaseGates, StopDrainsRunningPostAndSkipsCommitAndExtraction) {
    auto runtime = std::make_unique<PhaseRuntime>();
    auto& observed = *runtime;
    observed.HoldPost = true;
    auto entered = observed.Entered.get_future();
    auto engine = NEngine::NRuntime::EngineFactory::Create({.WorkerCount = 1}, std::move(runtime));
    engine->Start();
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
    auto stopped = std::async(std::launch::async, [&] { engine->Stop(); });
    EXPECT_TRUE(Progress(*engine, [&] { return engine->GetState() == NEngine::EEngineState::STOPPING; }));
    EXPECT_EQ(stopped.wait_for(5ms), std::future_status::timeout);
    observed.Release.release();
    ASSERT_EQ(stopped.wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(engine->GetLastError());
    EXPECT_EQ(observed.Events, (std::vector<int>{1}));
    EXPECT_EQ(observed.Draws.load(), 0);
    observed.HoldPost = false;
    observed.Events.clear();
    engine->Start();
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_TRUE(Progress(*engine, [&] { return observed.Draws.load() == 1; }));
    engine->Stop();
    EXPECT_FALSE(engine->GetLastError());
    EXPECT_EQ(observed.Events, (std::vector<int>{1, 2, 3, 4}));
}

TEST(EnginePhaseGates, BoundUserPostWaitsForSemanticCompletionAndRunsOnceBeforeCommit) {
    auto runtime = std::make_unique<PhaseRuntime>();
    auto& observed = *runtime;
    observed.HoldSemantic = true;
    auto semantic = observed.Semantic.get_future();
    auto engine = NEngine::NRuntime::EngineFactory::Create({.WorkerCount = 1}, std::move(runtime));
    engine->Start();
    const auto key = engine->TryBeginFrame();
    ASSERT_TRUE(key);
    struct Callback {
        PhaseRuntime& Runtime;
        NEngine::FrameKey Key;
    } callback{observed, *key};
    const auto invoke = +[](void* target) {
        auto& state = *static_cast<Callback*>(target);
        EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
        EXPECT_EQ(NCommon::FrameOriginScope::Current(),
                  (NCommon::FrameOrigin{state.Key.GetRunGeneration(), state.Key.GetFrameSequence()}));
        std::lock_guard lock{state.Runtime.Mutex};
        state.Runtime.Events.push_back(99);
    };
    ASSERT_TRUE(NEngine::NRuntime::EngineFactory::BindUserPostUpdate(*engine, *key, &callback, invoke));
    EXPECT_FALSE(NEngine::NRuntime::EngineFactory::BindUserPostUpdate(*engine, *key, &callback, invoke));
    ASSERT_TRUE(engine->ScheduleUserUpdate(*key, [] {}));
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(semantic.wait_for(2s), std::future_status::ready);
    auto completion = semantic.get();
    {
        std::lock_guard lock{observed.Mutex};
        EXPECT_EQ(observed.Events, (std::vector<int>{1}));
    }
    completion->Finish();
    ASSERT_TRUE(Progress(*engine, [&] { return observed.Draws.load() == 1; }));
    engine->Stop();
    EXPECT_EQ(observed.Events, (std::vector<int>{1, 2, 99, 3, 4}));
    observed.HoldSemantic = false;
    observed.Events.clear();
    engine->Start();
    EXPECT_FALSE(NEngine::NRuntime::EngineFactory::BindUserPostUpdate(*engine, *key, &callback, invoke));
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_TRUE(Progress(*engine, [&] { return observed.Draws.load() == 2; }));
    engine->Stop();
    EXPECT_EQ(observed.Events, (std::vector<int>{1, 2, 3, 4}));
}

TEST(EnginePhaseGates, BoundUserPostFailureCancelsCommitAndExtraction) {
    auto runtime = std::make_unique<PhaseRuntime>();
    auto& observed = *runtime;
    auto engine = NEngine::NRuntime::EngineFactory::Create({.WorkerCount = 1}, std::move(runtime));
    engine->Start();
    const auto key = engine->TryBeginFrame();
    ASSERT_TRUE(key);
    ASSERT_TRUE(NEngine::NRuntime::EngineFactory::BindUserPostUpdate(*engine, *key, nullptr, [](void*) {
        throw std::runtime_error{"user post-update failure"};
    }));
    ASSERT_TRUE(engine->ScheduleUserUpdate(*key, [] {}));
    ASSERT_TRUE(engine->Draw());
    ASSERT_TRUE(Progress(*engine, [&] {
        try {
            (void)engine->TryBeginFrame();
        } catch (...) {
        }
        return engine->GetLastError().has_value();
    }));
    engine->Stop();
    EXPECT_EQ(engine->GetLastError()->Message, "user post-update failure");
    EXPECT_EQ(observed.Events, (std::vector<int>{1, 2}));
    EXPECT_EQ(observed.Draws.load(), 0);
}

TEST(EnginePhaseGates, StopDrainsBoundUserPostBeforeTargetRetirementAndSkipsCommit) {
    auto runtime = std::make_unique<PhaseRuntime>();
    auto& observed = *runtime;
    auto engine = NEngine::NRuntime::EngineFactory::Create({.WorkerCount = 1}, std::move(runtime));
    struct Callback {
        std::promise<void> Entered;
        std::binary_semaphore Release{0};
        std::atomic_bool Finished = false;
        ~Callback() {
            Release.release();
        }
    } callback;
    auto entered = callback.Entered.get_future();
    engine->Start();
    const auto key = engine->TryBeginFrame();
    ASSERT_TRUE(key);
    ASSERT_TRUE(NEngine::NRuntime::EngineFactory::BindUserPostUpdate(*engine, *key, &callback, [](void* target) {
        auto& state = *static_cast<Callback*>(target);
        state.Entered.set_value();
        state.Release.acquire();
        state.Finished = true;
    }));
    ASSERT_TRUE(engine->ScheduleUserUpdate(*key, [] {}));
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
    auto stopped = std::async(std::launch::async, [&] { engine->Stop(); });
    EXPECT_TRUE(Progress(*engine, [&] { return engine->GetState() == NEngine::EEngineState::STOPPING; }));
    EXPECT_EQ(stopped.wait_for(5ms), std::future_status::timeout);
    callback.Release.release();
    ASSERT_EQ(stopped.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(callback.Finished.load());
    EXPECT_EQ(observed.Events, (std::vector<int>{1, 2}));
    EXPECT_EQ(observed.Draws.load(), 0);
}

TEST(EnginePhaseGates, StopSkipsBoundUserPostWhileSemanticUpdateIsPending) {
    auto runtime = std::make_unique<PhaseRuntime>();
    auto& observed = *runtime;
    observed.HoldSemantic = true;
    auto semantic = observed.Semantic.get_future();
    auto engine = NEngine::NRuntime::EngineFactory::Create({.WorkerCount = 1}, std::move(runtime));
    int calls = 0;
    engine->Start();
    const auto key = engine->TryBeginFrame();
    ASSERT_TRUE(key);
    ASSERT_TRUE(NEngine::NRuntime::EngineFactory::BindUserPostUpdate(*engine, *key, &calls, [](void* target) {
        ++*static_cast<int*>(target);
    }));
    ASSERT_TRUE(engine->ScheduleUserUpdate(*key, [] {}));
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(semantic.wait_for(2s), std::future_status::ready);
    engine->Stop();
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(observed.Events, (std::vector<int>{1}));
    EXPECT_EQ(observed.Draws.load(), 0);
}
} // namespace
