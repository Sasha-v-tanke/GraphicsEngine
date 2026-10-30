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

#include <engine/engine.h>
#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>
#include <lib/thread/job.h>
#include <lib/thread/task/task_system.h>
#include <tests/common/test_error.h>

namespace {

constexpr std::chrono::seconds TIMEOUT{5};

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
        engine.Stop();
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine.RunAsync([] {})); });
        engine.Start();
        auto next = engine.RunAsync([] { return 9; });
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
    engine.Stop();
    *shouldThrow = true;
    EXPECT_THROW(static_cast<void>(job.TryTake()), std::runtime_error);
    EXPECT_FALSE(job.IsReady());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(job.TryTake()); });
}

TEST(UserJobs, StopClosesAdmissionAndDrainsAcceptedQueuedWork) {
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
    EXPECT_TRUE(running.TryTake()->has_value());
    EXPECT_EQ(queued.TryTake()->value(), 8);
}

static_assert(!std::is_copy_constructible_v<NCommon::Job<int>>);
static_assert(std::is_nothrow_move_constructible_v<NCommon::Job<int>>);
static_assert(std::is_nothrow_move_assignable_v<NCommon::Job<void>>);
static_assert(!NCommon::UserJobFunction<int>);
using ReferenceFunction = int& (*)();
static_assert(!NCommon::UserJobFunction<ReferenceFunction>);

} // namespace
