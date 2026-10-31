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
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <engine/engine.h>
#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>
#include <lib/thread/task/task_system.h>
#include <lib/thread/user_task.h>
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

struct Payload {
    std::atomic_int& Destroyed;
    ~Payload() {
        ++Destroyed;
    }
};

struct Gate {
    std::promise<void> Entered;
    std::binary_semaphore Released{0};
    std::atomic_bool Opened{false};
    void Wait() {
        Entered.set_value();
        Released.acquire();
    }
    void Open() {
        if (!Opened.exchange(true)) {
            Released.release();
        }
    }
};

struct OpenOnExit {
    std::shared_ptr<Gate> Value;
    ~OpenOnExit() {
        Value->Open();
    }
};

NCommon::UserTask<int> ProbeTask(std::unique_ptr<Payload> payload, std::atomic_int& invoked) {
    ++invoked;
    EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
    EXPECT_TRUE(payload);
    co_return 42;
}

NCommon::UserTask<void> VoidTask(std::atomic_int& invoked) {
    ++invoked;
    co_return;
}

NCommon::UserTask<std::unique_ptr<int>> MoveResult(std::unique_ptr<int> value) {
    co_return std::move(value);
}

NCommon::UserTask<int> FailedTask(bool projectError) {
    if (projectError) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::IO_ERROR, "coroutine project failure");
    }
    throw std::runtime_error("coroutine failure");
    co_return 0;
}

NCommon::UserTask<int>
HeldTask(std::shared_ptr<Gate> gate, std::unique_ptr<Payload> payload, std::atomic_int& completed) {
    gate->Wait();
    EXPECT_TRUE(payload);
    ++completed;
    co_return 7;
}

NCommon::UserTask<void>
CooperativeTask(std::promise<void>& entered, std::unique_ptr<Payload> payload, std::atomic_bool& observedStop) {
    const std::stop_token token = co_await NCommon::UserTaskStopToken{};
    EXPECT_TRUE(payload);
    entered.set_value();
    const auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    observedStop = token.stop_requested();
    co_return;
}

TEST(UserTasks, LazyMoveOnlyOwnershipAndAbandonBeforeSpawnDestroyFrameExactlyOnce) {
    std::atomic_int invoked{0};
    std::atomic_int destroyed{0};
    {
        auto first = ProbeTask(std::make_unique<Payload>(destroyed), invoked);
        EXPECT_TRUE(first.IsValid());
        EXPECT_EQ(invoked.load(), 0);
        auto second = ProbeTask(std::make_unique<Payload>(destroyed), invoked);
        second = std::move(first);
        EXPECT_EQ(destroyed.load(), 1);
        NCommon::UserTask<int> moved{std::move(second)};
        EXPECT_TRUE(moved.IsValid());
        EXPECT_EQ(invoked.load(), 0);
    }
    EXPECT_EQ(destroyed.load(), 2);
    EXPECT_EQ(invoked.load(), 0);
}

TEST(UserTasks, SpawnRunsOnWorkerAndDestroysFrameBeforeRetainedResultIsPublished) {
    std::atomic_int invoked{0};
    std::atomic_int destroyed{0};
    NCommon::Job<int> result;
    {
        NEngine::Engine engine{{.WorkerCount = 1}};
        engine.Start();
        auto task = ProbeTask(std::make_unique<Payload>(destroyed), invoked);
        EXPECT_EQ(invoked.load(), 0);
        result = engine.Spawn(std::move(task));
        ASSERT_TRUE(WaitReady(result));
        EXPECT_EQ(destroyed.load(), 1);
        EXPECT_EQ(invoked.load(), 1);
        // NOLINTBEGIN(bugprone-use-after-move): verifies already-spawned task rejection.
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine.Spawn(std::move(task))); });
        // NOLINTEND(bugprone-use-after-move)
        engine.Stop();
    }
    EXPECT_EQ(result.TryTake()->value(), 42);
    EXPECT_EQ(destroyed.load(), 1);
}

TEST(UserTasks, VoidMoveOnlyAndCapturedErrorsUseTheJobContract) {
    std::atomic_int invoked{0};
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    auto nothing = engine.Spawn(VoidTask(invoked));
    auto unique = engine.Spawn(MoveResult(std::make_unique<int>(81)));
    auto failed = engine.Spawn(FailedTask(false));
    auto project = engine.Spawn(FailedTask(true));
    ASSERT_TRUE(WaitReady(nothing));
    ASSERT_TRUE(WaitReady(unique));
    ASSERT_TRUE(WaitReady(failed));
    ASSERT_TRUE(WaitReady(project));
    engine.Stop();
    EXPECT_TRUE(nothing.TryTake()->has_value());
    EXPECT_EQ(invoked.load(), 1);
    EXPECT_EQ(*unique.TryTake()->value(), 81);
    EXPECT_EQ(failed.TryTake()->error().Message, "coroutine failure");
    EXPECT_EQ(project.TryTake()->error().Code, NCommon::EError::IO_ERROR);
    EXPECT_FALSE(engine.GetLastError().has_value());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(unique.TryTake()); });
}

TEST(UserTasks, InvalidAndRejectedSpawnDoNotRunOrLeakCoroutines) {
    NEngine::Engine engine{{.WorkerCount = 1}};
    NCommon::UserTask<int> empty;
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine.Spawn(std::move(empty))); });
    std::atomic_int invoked{0};
    std::atomic_int destroyed{0};
    auto task = ProbeTask(std::make_unique<Payload>(destroyed), invoked);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine.Spawn(std::move(task))); });
    EXPECT_EQ(invoked.load(), 0);
    EXPECT_EQ(destroyed.load(), 1);
    engine.Start();
    engine.Stop();
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] {
        static_cast<void>(engine.Spawn(ProbeTask(std::make_unique<Payload>(destroyed), invoked)));
    });
    EXPECT_EQ(destroyed.load(), 2);
}

TEST(UserTasks, QueuedCancellationDestroysFrameWithoutRunningBodyOnSingleWorker) {
    std::atomic_int invoked{0};
    std::atomic_int destroyed{0};
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    const auto gate = std::make_shared<Gate>();
    std::future<void> entered = gate->Entered.get_future();
    const OpenOnExit guard{gate};
    auto busy = engine.RunAsync([gate] { gate->Wait(); });
    ASSERT_EQ(entered.wait_for(TIMEOUT), std::future_status::ready);
    auto queued = engine.Spawn(ProbeTask(std::make_unique<Payload>(destroyed), invoked));
    queued.RequestCancel();
    ASSERT_TRUE(WaitReady(queued));
    EXPECT_EQ(queued.TryTake()->error().Code, NCommon::EError::CANCELLED);
    EXPECT_EQ(invoked.load(), 0);
    EXPECT_EQ(destroyed.load(), 1);
    gate->Open();
    ASSERT_TRUE(WaitReady(busy));
    engine.Stop();
}

TEST(UserTasks, DroppedJobDoesNotDestroyRunningFrameAndShutdownDrainsIt) {
    std::atomic_int completed{0};
    std::atomic_int destroyed{0};
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    const auto gate = std::make_shared<Gate>();
    std::future<void> entered = gate->Entered.get_future();
    const OpenOnExit guard{gate};
    auto result = engine.Spawn(HeldTask(gate, std::make_unique<Payload>(destroyed), completed));
    ASSERT_EQ(entered.wait_for(TIMEOUT), std::future_status::ready);
    result = {};
    EXPECT_EQ(destroyed.load(), 0);
    gate->Open();
    engine.Stop();
    EXPECT_EQ(completed.load(), 1);
    EXPECT_EQ(destroyed.load(), 1);
}

TEST(UserTasks, StopCancelsQueuedCoroutineAndCooperativelyStopsRunningCoroutineAcrossRestart) {
    std::promise<void> entered;
    std::atomic_bool observedStop{false};
    std::atomic_int invoked{0};
    std::atomic_int destroyed{0};
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    std::future<void> started = entered.get_future();
    auto running = engine.Spawn(CooperativeTask(entered, std::make_unique<Payload>(destroyed), observedStop));
    ASSERT_EQ(started.wait_for(TIMEOUT), std::future_status::ready);
    auto queued = engine.Spawn(ProbeTask(std::make_unique<Payload>(destroyed), invoked));
    engine.Stop();
    EXPECT_TRUE(observedStop.load());
    EXPECT_EQ(destroyed.load(), 2);
    EXPECT_EQ(invoked.load(), 0);
    engine.Start();
    running.RequestCancel();
    auto current = engine.Spawn(VoidTask(invoked));
    ASSERT_TRUE(WaitReady(current));
    engine.Stop();
    EXPECT_TRUE(current.TryTake()->has_value());
    EXPECT_EQ(running.TryTake()->error().Code, NCommon::EError::CANCELLED);
    EXPECT_EQ(queued.TryTake()->error().Code, NCommon::EError::CANCELLED);
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

TEST(UserTasks, CoroutineExecutionDoesNotReserveOrGateFrameSlotsForTwelveFrames) {
    std::atomic_int completed{0};
    std::atomic_int destroyed{0};
    auto frames = std::make_unique<CountingFrames>();
    CountingFrames& counter = *frames;
    auto engine = NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = 1, .WorkerCount = 2}, std::move(frames));
    engine->Start();
    const auto gate = std::make_shared<Gate>();
    std::future<void> entered = gate->Entered.get_future();
    const OpenOnExit guard{gate};
    auto result = engine->Spawn(HeldTask(gate, std::make_unique<Payload>(destroyed), completed));
    ASSERT_EQ(entered.wait_for(TIMEOUT), std::future_status::ready);
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
        EXPECT_FALSE(result.IsReady());
    }
    gate->Open();
    ASSERT_TRUE(WaitReady(result));
    engine->Stop();
    EXPECT_EQ(result.TryTake()->value(), 7);
    EXPECT_EQ(completed.load(), 1);
    EXPECT_EQ(destroyed.load(), 1);
}

struct ThrowOnMove {
    ThrowOnMove() = default;
    ThrowOnMove(ThrowOnMove&&) noexcept(false) {
        throw std::runtime_error("co_return move failed");
    }
};

NCommon::UserTask<ThrowOnMove> ThrowingReturn(std::unique_ptr<Payload> payload) {
    EXPECT_TRUE(payload);
    co_return ThrowOnMove{};
}

NCommon::UserTask<void> VoidFailure() {
    throw std::bad_alloc{};
    co_return;
}

TEST(UserTasks, ReturnConstructionAndVoidExceptionsBecomeFailedJobsAndReleaseFrames) {
    std::atomic_int destroyed{0};
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    auto moved = engine.Spawn(ThrowingReturn(std::make_unique<Payload>(destroyed)));
    auto empty = engine.Spawn(VoidFailure());
    ASSERT_TRUE(WaitReady(moved));
    ASSERT_TRUE(WaitReady(empty));
    EXPECT_EQ(destroyed.load(), 1);
    EXPECT_EQ(moved.TryTake()->error().Message, "co_return move failed");
    EXPECT_EQ(empty.TryTake()->error().Code, NCommon::EError::OUT_OF_MEMORY);
    engine.Stop();
}

template<typename Awaitable>
concept AcceptedUserAwaiter = requires(NCommon::NUserTaskInternal::PromiseBase& promise, Awaitable&& awaitable) {
    promise.await_transform(std::forward<Awaitable>(awaitable));
};
static_assert(AcceptedUserAwaiter<NCommon::UserTaskStopToken>);
static_assert(!AcceptedUserAwaiter<std::suspend_always>);
static_assert(!AcceptedUserAwaiter<NCommon::Job<int>>);

static_assert(!std::is_copy_constructible_v<NCommon::UserTask<int>>);
static_assert(std::is_nothrow_move_constructible_v<NCommon::UserTask<int>>);
static_assert(std::is_nothrow_move_assignable_v<NCommon::UserTask<void>>);

} // namespace
