#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <engine/engine.h>
#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>
#include <lib/thread/job.h>
#include <lib/thread/task/task_system.h>
#include <tests/common/test_error.h>

namespace {

constexpr std::chrono::seconds TIMEOUT{5};

template<typename T>
bool WaitReady(const NCommon::Job<T>& job) {
    const auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!job.IsReady() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return job.IsReady();
}

struct ControlledJob {
    std::binary_semaphore Release{0};
    std::atomic_bool Opened{false};
    std::promise<void> Started;

    void Wait() {
        Started.set_value();
        Release.acquire();
    }

    void Open() {
        if (!Opened.exchange(true)) {
            Release.release();
        }
    }
};

struct OpenJobOnExit {
    std::shared_ptr<ControlledJob> Gate;
    ~OpenJobOnExit() {
        Gate->Open();
    }
};

TEST(UserJobs, ReturnsImmediatelyAndPollsWithoutBlockingWithOneWorker) {
    const auto gate = std::make_shared<ControlledJob>();
    std::future<void> started = gate->Started.get_future();
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    const OpenJobOnExit guard{gate};
    NCommon::Job<int> first = engine.RunAsync([gate] {
        gate->Wait();
        return 10;
    });
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    NCommon::Job<int> second = engine.RunAsync([] { return 20; });
    EXPECT_FALSE(first.IsReady());
    EXPECT_FALSE(first.TryTake().has_value());
    EXPECT_FALSE(second.IsReady());
    EXPECT_FALSE(second.TryTake().has_value());
    gate->Open();
    ASSERT_TRUE(WaitReady(first));
    ASSERT_TRUE(WaitReady(second));
    engine.Stop();
    ASSERT_TRUE(first.IsReady());
    ASSERT_TRUE(second.IsReady());
    EXPECT_EQ(first.TryTake()->value(), 10);
    EXPECT_EQ(second.TryTake()->value(), 20);
}

TEST(UserJobs, SupportsVoidAndMoveOnlyCallableAndNonAssignableResult) {
    struct Value {
        explicit Value(int value)
            : Number(value) {
        }
        Value(Value&&) = default;
        Value(const Value&) = delete;
        Value& operator=(Value&&) = delete;
        Value& operator=(const Value&) = delete;
        int Number;
    };
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    std::atomic_bool ran{false};
    NCommon::Job<void> nothing = engine.RunAsync([&ran] { ran = true; });
    auto unique = engine.RunAsync([value = std::make_unique<int>(42)]() mutable { return std::move(value); });
    auto nonAssignable = engine.RunAsync([] { return Value{17}; });
    ASSERT_TRUE(WaitReady(nothing));
    ASSERT_TRUE(WaitReady(unique));
    ASSERT_TRUE(WaitReady(nonAssignable));
    engine.Stop();
    ASSERT_TRUE(nothing.TryTake()->has_value());
    EXPECT_TRUE(ran.load());
    auto result = unique.TryTake();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(*result->value(), 42);
    EXPECT_EQ(nonAssignable.TryTake()->value().Number, 17);
}

TEST(UserJobs, CapturesErrorsWithoutFailingEngineAndConsumesEachResultOnce) {
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    auto project = engine.RunAsync([]() -> int { GRAPHICS_ENGINE_THROW(NCommon::EError::IO_ERROR, "asset failed"); });
    auto standard = engine.RunAsync([]() -> int { throw std::runtime_error("compute failed"); });
    auto unknown = engine.RunAsync([]() -> void { throw 7; });
    auto allocation = engine.RunAsync([]() -> int { throw std::bad_alloc{}; });
    ASSERT_TRUE(WaitReady(project));
    ASSERT_TRUE(WaitReady(standard));
    ASSERT_TRUE(WaitReady(unknown));
    ASSERT_TRUE(WaitReady(allocation));
    engine.Stop();
    ASSERT_TRUE(project.IsReady());
    auto error = project.TryTake();
    ASSERT_TRUE(error.has_value());
    ASSERT_FALSE(error->has_value());
    EXPECT_EQ(error->error().Code, NCommon::make_error_code(NCommon::EError::IO_ERROR));
    EXPECT_EQ(error->error().Message, "asset failed");
    EXPECT_EQ(standard.TryTake()->error().Message, "compute failed");
    EXPECT_EQ(unknown.TryTake()->error().Code, NCommon::make_error_code(NCommon::EError::UNKNOWN));
    EXPECT_EQ(allocation.TryTake()->error().Code, NCommon::make_error_code(NCommon::EError::OUT_OF_MEMORY));
    EXPECT_FALSE(engine.GetLastError().has_value());
    EXPECT_FALSE(project.IsReady());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(project.TryTake()); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(unknown.TryTake()); });
}

TEST(UserJobs, RejectsInvalidMovedFromAndConsumedHandles) {
    NCommon::Job<int> empty;
    EXPECT_FALSE(empty.IsReady());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(empty.TryTake()); });
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    auto original = engine.RunAsync([] { return 5; });
    NCommon::Job<int> moved = std::move(original);
    EXPECT_FALSE(original.IsReady()); // NOLINT(bugprone-use-after-move): moved-from contract.
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(original.TryTake()); });
    ASSERT_TRUE(WaitReady(moved));
    engine.Stop();
    empty = std::move(moved);
    EXPECT_EQ(empty.TryTake()->value(), 5);
    EXPECT_FALSE(empty.IsReady());
    EXPECT_FALSE(moved.IsReady()); // NOLINT(bugprone-use-after-move): moved-from contract.
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(empty.TryTake()); });
}

TEST(UserJobs, DroppedHandleDoesNotCancelWorkOrRetainCallableAndResult) {
    const auto gate = std::make_shared<ControlledJob>();
    std::future<void> started = gate->Started.get_future();
    const auto payload = std::make_shared<int>(3);
    std::weak_ptr<int> lifetime = payload;
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    const OpenJobOnExit guard{gate};
    std::atomic_int invoked{0};
    {
        auto job = engine.RunAsync([gate, retained = payload, &invoked] {
            gate->Wait();
            ++invoked;
            return retained;
        });
        ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
        EXPECT_FALSE(job.IsReady());
    }
    gate->Open();
    engine.Stop();
    EXPECT_EQ(invoked.load(), 1);
    EXPECT_EQ(payload.use_count(), 1);
    EXPECT_FALSE(lifetime.expired());
}

TEST(UserJobs, SubmissionRequiresRunningRuntimeAndResultsSurviveRestartAndDestruction) {
    NCommon::Job<std::string> old;
    {
        NEngine::Engine engine{{.WorkerCount = 1}};
        NTest::ExpectError(NCommon::EError::INVALID_STATE,
                           [&] { static_cast<void>(engine.RunAsync([] { return 1; })); });
        engine.Start();
        old = engine.RunAsync([] { return std::string{"first run"}; });
        ASSERT_TRUE(WaitReady(old));
        engine.Stop();
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine.RunAsync([] {})); });
        engine.Start();
        auto next = engine.RunAsync([] { return 9; });
        ASSERT_TRUE(WaitReady(next));
        engine.Stop();
        EXPECT_EQ(next.TryTake()->value(), 9);
    }
    ASSERT_TRUE(old.IsReady());
    EXPECT_EQ(old.TryTake()->value(), "first run");
}

class CountingFrames final: public NEngine::NRuntime::IFrameRuntime {
public:
    void Update(const NEngine::NRuntime::FrameContext&) override {
    }
    void Draw(const NEngine::NRuntime::FrameContext&) override {
        {
            std::lock_guard lock{m_mutex};
            ++m_count;
        }
        m_changed.notify_all();
    }
    bool WaitFor(std::size_t count) {
        std::unique_lock lock{m_mutex};
        return m_changed.wait_for(lock, TIMEOUT, [&] { return m_count >= count; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::size_t m_count = 0;
};

TEST(UserJobs, IndependentJobOutlivesTwelveFramesWithoutReservingFrameSlot) {
    auto frames = std::make_unique<CountingFrames>();
    CountingFrames& counter = *frames;
    const auto gate = std::make_shared<ControlledJob>();
    std::future<void> started = gate->Started.get_future();
    auto engine = NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = 1, .WorkerCount = 2}, std::move(frames));
    engine->Start();
    const OpenJobOnExit guard{gate};
    auto background = engine->RunAsync([gate] {
        gate->Wait();
        return 81;
    });
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    const auto eventually = [&engine](bool draw) {
        const auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
        while (std::chrono::steady_clock::now() < deadline) {
            const std::uint64_t version = engine->GetProgressVersion();
            if (draw ? engine->Draw() : engine->Update()) {
                return true;
            }
            engine->WaitForProgress(version, std::chrono::milliseconds{1});
        }
        return false;
    };
    for (std::size_t frame = 1; frame <= 12; ++frame) {
        ASSERT_TRUE(eventually(false));
        ASSERT_TRUE(eventually(true));
        ASSERT_TRUE(counter.WaitFor(frame));
        EXPECT_FALSE(background.IsReady());
        EXPECT_FALSE(background.TryTake().has_value());
    }
    gate->Open();
    ASSERT_TRUE(WaitReady(background));
    engine->Stop();
    EXPECT_EQ(background.TryTake()->value(), 81);
}

TEST(UserJobs, RvalueOnlyCallableRunsOnComputeWorker) {
    struct Function {
        int operator()() && {
            EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
            return 2;
        }
    };
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    auto job = engine.RunAsync(Function{});
    ASSERT_TRUE(WaitReady(job));
    engine.Stop();
    EXPECT_EQ(job.TryTake()->value(), 2);
}

TEST(UserJobs, ThrowingResultMoveBecomesFailedJob) {
    struct Value {
        Value() = default;
        Value(Value&&) noexcept(false) {
            throw std::runtime_error("result move failed");
        }
    };
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    auto job = engine.RunAsync([] { return Value{}; });
    ASSERT_TRUE(WaitReady(job));
    engine.Stop();
    auto result = job.TryTake();
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().Message, "result move failed");
}

TEST(UserJobs, ThrowingConsumerMoveStillConsumesResult) {
    struct Value {
        std::shared_ptr<bool> Throw;
        explicit Value(std::shared_ptr<bool> shouldThrow)
            : Throw(std::move(shouldThrow)) {
        }
        Value(Value&& other) noexcept(false)
            : Throw(std::move(other.Throw)) {
            if (*Throw) {
                throw std::runtime_error("consumer move failed");
            }
        }
    };
    const auto shouldThrow = std::make_shared<bool>(false);
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    auto job = engine.RunAsync([shouldThrow] { return Value{shouldThrow}; });
    ASSERT_TRUE(WaitReady(job));
    engine.Stop();
    *shouldThrow = true;
    EXPECT_THROW(static_cast<void>(job.TryTake()), std::runtime_error);
    EXPECT_FALSE(job.IsReady());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(job.TryTake()); });
}

TEST(UserJobs, StopClosesAdmissionAndCancelsQueuedWork) {
    const auto gate = std::make_shared<ControlledJob>();
    std::future<void> started = gate->Started.get_future();
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    const OpenJobOnExit guard{gate};
    auto running = engine.RunAsync([gate] { gate->Wait(); });
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    auto queued = engine.RunAsync([] { return 8; });
    std::thread stop{[&engine] { engine.Stop(); }};
    const auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (engine.GetState() == NEngine::EEngineState::RUNNING && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(engine.GetState(), NEngine::EEngineState::STOPPING);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine.RunAsync([] {})); });
    gate->Open();
    stop.join();
    EXPECT_EQ(running.TryTake()->error().Code, NCommon::EError::CANCELLED);
    EXPECT_EQ(queued.TryTake()->error().Code, NCommon::EError::CANCELLED);
}

static_assert(!std::is_copy_constructible_v<NCommon::Job<int>>);
static_assert(std::is_nothrow_move_constructible_v<NCommon::Job<int>>);
static_assert(std::is_nothrow_move_assignable_v<NCommon::Job<void>>);
static_assert(!NCommon::UserJobFunction<int>);
using ReferenceFunction = int& (*)();
static_assert(!NCommon::UserJobFunction<ReferenceFunction>);

} // namespace

namespace {

TEST(UserJobCancellation, QueuedCancellationPublishesWithoutAvailableWorkerAndReleasesCaptures) {
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    const auto gate = std::make_shared<ControlledJob>();
    std::future<void> started = gate->Started.get_future();
    const OpenJobOnExit guard{gate};
    auto busy = engine.RunAsync([gate] { gate->Wait(); });
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    std::atomic_bool invoked{false};
    auto payload = std::make_shared<int>(9);
    const std::weak_ptr<int> lifetime = payload;
    auto queued = engine.RunAsync([retained = std::move(payload), &invoked] {
        invoked = true;
        return *retained;
    });
    queued.RequestCancel();
    ASSERT_TRUE(WaitReady(queued));
    EXPECT_EQ(queued.TryTake()->error().Code, NCommon::EError::CANCELLED);
    EXPECT_FALSE(invoked.load());
    EXPECT_TRUE(lifetime.expired());
    gate->Open();
    ASSERT_TRUE(WaitReady(busy));
    engine.Stop();
    EXPECT_FALSE(invoked.load());
}

TEST(UserJobCancellation, StopTokenCallbacksAreAsynchronousAndDoNotHoldRuntimeLocks) {
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    const auto callbackGate = std::make_shared<ControlledJob>();
    std::future<void> callbackEntered = callbackGate->Started.get_future();
    const OpenJobOnExit guard{callbackGate};
    std::promise<void> started;
    std::future<void> startedFuture = started.get_future();
    const std::thread::id caller = std::this_thread::get_id();
    auto job = engine.RunAsync([&engine, &started, callbackGate, caller](const std::stop_token& token) {
        std::stop_callback callback{token, [&engine, callbackGate, caller] {
                                        EXPECT_NE(std::this_thread::get_id(), caller);
                                        EXPECT_EQ(engine.GetState(), NEngine::EEngineState::RUNNING);
                                        callbackGate->Wait();
                                    }};
        started.set_value();
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
        return std::make_unique<int>(1);
    });
    ASSERT_EQ(startedFuture.wait_for(TIMEOUT), std::future_status::ready);
    job.RequestCancel(); // Must return before the deliberately blocked stop_callback.
    ASSERT_EQ(callbackEntered.wait_for(TIMEOUT), std::future_status::ready);
    EXPECT_FALSE(job.IsReady());
    callbackGate->Open();
    ASSERT_TRUE(WaitReady(job));
    EXPECT_EQ(job.TryTake()->error().Code, NCommon::EError::CANCELLED);
    engine.Stop();
}

TEST(UserJobCancellation, StopSignalsRunningJobAndPreservesOldResultAcrossRestart) {
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    std::promise<void> started;
    std::future<void> entered = started.get_future();
    auto old = engine.RunAsync([&started](const std::stop_token& token) {
        started.set_value();
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
    });
    ASSERT_EQ(entered.wait_for(TIMEOUT), std::future_status::ready);
    engine.Stop();
    ASSERT_TRUE(old.IsReady());
    engine.Start();
    old.RequestCancel(); // Cannot affect the new run.
    auto current = engine.RunAsync([](const std::stop_token& token) { return token.stop_requested(); });
    ASSERT_TRUE(WaitReady(current));
    current.RequestCancel(); // Terminal success is immutable.
    EXPECT_FALSE(current.TryTake()->value());
    engine.Stop();
    EXPECT_EQ(old.TryTake()->error().Code, NCommon::EError::CANCELLED);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { old.RequestCancel(); });
}

TEST(UserJobCancellation, ConcurrentCompletionCancellationAndStopStayTerminal) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        NEngine::Engine engine{{.WorkerCount = workers}};
        engine.Start();
        std::vector<NCommon::Job<int>> jobs;
        jobs.reserve(128);
        for (int index = 0; index < 128; ++index) {
            jobs.push_back(engine.RunAsync([index](const std::stop_token&) { return index; }));
        }
        std::thread cancel{[&jobs] {
            for (NCommon::Job<int>& job: jobs) {
                job.RequestCancel();
            }
        }};
        engine.Stop();
        cancel.join();
        for (NCommon::Job<int>& job: jobs) {
            ASSERT_TRUE(job.IsReady());
            const auto result = job.TryTake();
            ASSERT_TRUE(result.has_value());
            EXPECT_TRUE(result->has_value() || result->error().Code == NCommon::EError::CANCELLED);
        }
    }
}

TEST(UserJobCancellation, FailedAbandonedJobReportsOnceAndObservedFailureDoesNotReport) {
    testing::internal::CaptureStderr();
    {
        NEngine::Engine engine{{.WorkerCount = 1}};
        engine.Start();
        auto observed = engine.RunAsync([] { throw std::runtime_error("observed-failure"); });
        ASSERT_TRUE(WaitReady(observed));
        EXPECT_FALSE(observed.TryTake()->has_value());
        auto abandoned = engine.RunAsync([] { throw std::runtime_error("abandoned-failure"); });
        ASSERT_TRUE(WaitReady(abandoned));
        abandoned = {};
        engine.Stop();
    }
    const std::string diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_EQ(diagnostic.find("observed-failure"), std::string::npos);
    const std::size_t position = diagnostic.find("abandoned-failure");
    ASSERT_NE(position, std::string::npos);
    EXPECT_EQ(diagnostic.find("abandoned-failure", position + 1), std::string::npos);
}

} // namespace

namespace {

TEST(UserJobCancellation, FailureWinsConcurrentCancellationAndDroppedHandleStillFinishes) {
    testing::internal::CaptureStderr();
    {
        NEngine::Engine engine{{.WorkerCount = 1}};
        engine.Start();
        const auto gate = std::make_shared<ControlledJob>();
        std::future<void> started = gate->Started.get_future();
        const OpenJobOnExit guard{gate};
        auto failed = engine.RunAsync([gate]() -> int {
            gate->Wait();
            throw std::runtime_error("failure-during-cancel");
        });
        ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
        failed.RequestCancel();
        failed = {};
        gate->Open();
        engine.Stop();
    }
    const std::string diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_NE(diagnostic.find("failure-during-cancel"), std::string::npos);
}

TEST(UserJobCancellation, ReentrantCaptureDestructionDuringQueuedCancelRunsOutsideEngineLocks) {
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    const auto gate = std::make_shared<ControlledJob>();
    std::future<void> started = gate->Started.get_future();
    const OpenJobOnExit guard{gate};
    auto busy = engine.RunAsync([gate] { gate->Wait(); });
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    std::atomic_int released{0};
    std::shared_ptr<int> capture{new int{1}, [&engine, &released](const int* value) {
                                     EXPECT_EQ(engine.GetState(), NEngine::EEngineState::RUNNING);
                                     ++released;
                                     delete value;
                                 }};
    auto queued = engine.RunAsync([capture = std::move(capture)] { return *capture; });
    queued.RequestCancel();
    ASSERT_TRUE(WaitReady(queued));
    EXPECT_EQ(released.load(), 1);
    EXPECT_EQ(queued.TryTake()->error().Code, NCommon::EError::CANCELLED);
    gate->Open();
    ASSERT_TRUE(WaitReady(busy));
    engine.Stop();
}

TEST(UserJobCancellation, ConcurrentAdmissionAndStopNeverLeavePendingHandles) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        NEngine::Engine engine{{.WorkerCount = workers}};
        engine.Start();
        std::vector<NCommon::Job<int>> accepted;
        std::atomic_bool started{false};
        std::atomic_int rejected{0};
        std::thread submitter{[&] {
            started = true;
            for (int index = 0; index < 128; ++index) {
                try {
                    accepted.push_back(engine.RunAsync([index] { return index; }));
                } catch (const NCommon::Exception& exception) {
                    EXPECT_EQ(exception.code(), NCommon::EError::INVALID_STATE);
                    ++rejected;
                }
            }
        }};
        while (!started.load()) {
            std::this_thread::yield();
        }
        engine.Stop();
        submitter.join();
        EXPECT_EQ(accepted.size() + static_cast<std::size_t>(rejected.load()), 128U);
        for (NCommon::Job<int>& job: accepted) {
            ASSERT_TRUE(job.IsReady());
            static_cast<void>(job.TryTake());
        }
    }
}

} // namespace

namespace {

TEST(UserJobCancellation, FrameFailureClosesUserScopeBeforeQueuedCallableCanStart) {
    class FailingFrames final: public NEngine::NRuntime::IFrameRuntime {
    public:
        explicit FailingFrames(std::shared_ptr<ControlledJob> gate)
            : m_gate(std::move(gate)) {
        }
        void Update(const NEngine::NRuntime::FrameContext&) override {
            m_gate->Wait();
            throw std::runtime_error("frame failed");
        }
        void Draw(const NEngine::NRuntime::FrameContext&) override {
        }

    private:
        std::shared_ptr<ControlledJob> m_gate;
    };
    const auto gate = std::make_shared<ControlledJob>();
    std::future<void> entered = gate->Started.get_future();
    auto engine = NEngine::NRuntime::EngineFactory::Create({.WorkerCount = 1}, std::make_unique<FailingFrames>(gate));
    engine->Start();
    const OpenJobOnExit guard{gate};
    ASSERT_TRUE(engine->Update());
    ASSERT_EQ(entered.wait_for(TIMEOUT), std::future_status::ready);
    std::atomic_bool invoked{false};
    auto queued = engine->RunAsync([&invoked] { invoked = true; });
    gate->Open();
    ASSERT_TRUE(WaitReady(queued));
    EXPECT_EQ(queued.TryTake()->error().Code, NCommon::EError::CANCELLED);
    EXPECT_FALSE(invoked.load());
    engine->Stop();
    EXPECT_TRUE(engine->GetLastError().has_value());
}

} // namespace
