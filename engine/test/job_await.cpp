#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <engine/engine.h>
#include <gtest/gtest.h>
#include <lib/thread/job.h>
#include <lib/thread/task/task_system.h>
#include <tests/common/test_error.h>

namespace {

constexpr std::chrono::seconds TIMEOUT{5};

template<typename Predicate>
bool Eventually(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return predicate();
}

// Retain an admitted result before starting its producer, so ordering does not depend on worker timing.
template<typename Function>
auto Pending(Function&& function, std::shared_ptr<NCommon::NJobInternal::JobControl>& source) {
    return NCommon::NJobInternal::JobFactory::Run(
            [&source](const std::shared_ptr<NCommon::NJobInternal::JobControl>& state) { source = state; },
            std::forward<Function>(function));
}

template<typename T>
auto SpawnObserved(NEngine::Engine& engine,
                   NCommon::UserTask<T>&& task,
                   std::shared_ptr<NCommon::NJobInternal::JobControl>& owner) {
    return NCommon::NJobInternal::JobFactory::Spawn(
            [&engine, &owner](const std::shared_ptr<NCommon::NJobInternal::JobControl>& state) {
                owner = state;
                engine.SubmitUserJob(state);
            },
            std::move(task));
}

bool IsWaiting(const std::shared_ptr<NCommon::NJobInternal::JobControl>& owner) {
    return owner->Phase.load() == NCommon::NJobInternal::EJobPhase::WAITING;
}

NCommon::UserTask<int> AwaitValue(NCommon::Job<int> source, std::atomic_int& resumed) {
    const int value = co_await std::move(source);
    EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
    ++resumed;
    co_return value + 1;
}

NCommon::UserTask<void> AwaitVoid(NCommon::Job<void> source) {
    co_await std::move(source);
}

NCommon::UserTask<std::unique_ptr<int>> AwaitUnique(NCommon::Job<std::unique_ptr<int>> source) {
    co_return co_await std::move(source);
}

TEST(JobAwait, ReadyBeforeAwaitSupportsVoidMoveOnlyAndInvalidConsumption) {
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    auto source = engine.RunAsync([] { return 41; });
    auto nothing = engine.RunAsync([] {});
    auto unique = engine.RunAsync([] { return std::make_unique<int>(73); });
    ASSERT_TRUE(Eventually([&] { return source.IsReady() && nothing.IsReady() && unique.IsReady(); }));
    std::atomic_int resumed{0};
    auto result = engine.Spawn(AwaitValue(std::move(source), resumed));
    auto empty = engine.Spawn(AwaitVoid(std::move(nothing)));
    auto moved = engine.Spawn(AwaitUnique(std::move(unique)));
    auto invalid = engine.Spawn(AwaitValue({}, resumed));
    ASSERT_TRUE(
            Eventually([&] { return result.IsReady() && empty.IsReady() && moved.IsReady() && invalid.IsReady(); }));
    EXPECT_EQ(result.TryTake()->value(), 42);
    EXPECT_TRUE(empty.TryTake()->has_value());
    EXPECT_EQ(*moved.TryTake()->value(), 73);
    EXPECT_EQ(invalid.TryTake()->error().Code, NCommon::EError::INVALID_STATE);
    EXPECT_EQ(resumed.load(), 1);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(result.TryTake()); });
    engine.Stop();
}

TEST(JobAwait, SuspendedCoroutineReleasesSingleWorkerAndCompletionResumesExactlyOnce) {
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    std::shared_ptr<NCommon::NJobInternal::JobControl> source;
    std::shared_ptr<NCommon::NJobInternal::JobControl> owner;
    std::atomic_int resumed{0};
    auto result = SpawnObserved(engine, AwaitValue(Pending([] { return 41; }, source), resumed), owner);
    ASSERT_TRUE(Eventually([&] { return IsWaiting(owner); }));
    for (int index = 0; index < 16; ++index) {
        auto independent = engine.RunAsync([index] { return index; });
        ASSERT_TRUE(Eventually([&] { return independent.IsReady(); }));
        EXPECT_EQ(independent.TryTake()->value(), index);
        EXPECT_FALSE(result.IsReady());
    }
    source->Run();
    ASSERT_TRUE(Eventually([&] { return result.IsReady(); }));
    source->Run();
    EXPECT_EQ(resumed.load(), 1);
    EXPECT_EQ(result.TryTake()->value(), 42);
    engine.Stop();
}

TEST(JobAwait, FailedAndCancelledSourcesPropagateProjectErrors) {
    NEngine::Engine engine{{.WorkerCount = 2}};
    engine.Start();
    std::shared_ptr<NCommon::NJobInternal::JobControl> source;
    std::shared_ptr<NCommon::NJobInternal::JobControl> owner;
    std::atomic_int resumed{0};
    auto failed = engine.Spawn(AwaitValue(
            engine.RunAsync([]() -> int { GRAPHICS_ENGINE_THROW(NCommon::EError::IO_ERROR, "awaited failure"); }),
            resumed));
    auto cancelled = SpawnObserved(engine, AwaitValue(Pending([] { return 3; }, source), resumed), owner);
    ASSERT_TRUE(Eventually([&] { return IsWaiting(owner); }));
    source->RequestCancel();
    source->DispatchCancel();
    ASSERT_TRUE(Eventually([&] { return failed.IsReady() && cancelled.IsReady(); }));
    EXPECT_EQ(failed.TryTake()->error().Message, "awaited failure");
    EXPECT_EQ(cancelled.TryTake()->error().Code, NCommon::EError::CANCELLED);
    EXPECT_EQ(resumed.load(), 0);
    engine.Stop();
}

struct FrameProbe {
    std::atomic_int& Destroyed;
    ~FrameProbe() {
        ++Destroyed;
    }
};

NCommon::UserTask<int>
AwaitWithLifetime(NCommon::Job<int> source, std::unique_ptr<FrameProbe> probe, std::atomic_int& resumed) {
    EXPECT_TRUE(probe);
    const int value = co_await std::move(source);
    ++resumed;
    co_return value;
}

TEST(JobAwait, CancelSuspendedAndStopInvalidateLateNotificationsAcrossRestart) {
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    std::atomic_int resumed{0};
    std::atomic_int destroyed{0};
    std::vector<std::shared_ptr<NCommon::NJobInternal::JobControl>> sources;
    for (int iteration = 0; iteration < 2; ++iteration) {
        std::shared_ptr<NCommon::NJobInternal::JobControl> source;
        std::shared_ptr<NCommon::NJobInternal::JobControl> owner;
        auto pending = Pending([] { return 42; }, source);
        sources.push_back(source);
        auto result =
                SpawnObserved(engine,
                              AwaitWithLifetime(std::move(pending), std::make_unique<FrameProbe>(destroyed), resumed),
                              owner);
        ASSERT_TRUE(Eventually([&] { return IsWaiting(owner); }));
        if (iteration == 0) {
            result.RequestCancel();
            ASSERT_TRUE(Eventually([&] { return result.IsReady(); }));
        } else {
            engine.Stop();
        }
        EXPECT_EQ(result.TryTake()->error().Code, NCommon::EError::CANCELLED);
        EXPECT_EQ(destroyed.load(), iteration + 1);
    }
    engine.Start();
    for (const auto& source: sources) {
        source->Run();
    }
    auto current = engine.RunAsync([] { return 7; });
    ASSERT_TRUE(Eventually([&] { return current.IsReady(); }));
    EXPECT_EQ(current.TryTake()->value(), 7);
    EXPECT_EQ(resumed.load(), 0);
    EXPECT_EQ(destroyed.load(), 2);
    engine.Stop();
}

NCommon::UserTask<int> AwaitChain(std::vector<NCommon::Job<int>> sources, std::atomic_int& resumed) {
    int sum = 0;
    for (NCommon::Job<int>& source: sources) {
        sum += co_await std::move(source);
        ++resumed;
    }
    co_return sum;
}

TEST(JobAwait, SuccessiveAwaitsDoNotReuseEarlierWakeupsAndDroppedHandleRetainsFrame) {
    NEngine::Engine engine{{.WorkerCount = 2}};
    engine.Start();
    std::vector<NCommon::Job<int>> jobs;
    std::vector<std::shared_ptr<NCommon::NJobInternal::JobControl>> sources;
    for (int index = 0; index < 32; ++index) {
        std::shared_ptr<NCommon::NJobInternal::JobControl> source;
        jobs.push_back(Pending([] { return 1; }, source));
        sources.push_back(std::move(source));
    }
    std::shared_ptr<NCommon::NJobInternal::JobControl> owner;
    std::atomic_int resumed{0};
    auto result = SpawnObserved(engine, AwaitChain(std::move(jobs), resumed), owner);
    ASSERT_TRUE(Eventually([&] { return IsWaiting(owner); }));
    for (std::size_t index = 0; index < sources.size(); ++index) {
        sources[index]->Run();
        ASSERT_TRUE(Eventually([&] { return resumed.load() == static_cast<int>(index + 1); }));
        if (index + 1 < sources.size()) {
            ASSERT_TRUE(Eventually([&] { return IsWaiting(owner); }));
            EXPECT_FALSE(result.IsReady());
        }
    }
    ASSERT_TRUE(Eventually([&] { return result.IsReady(); }));
    EXPECT_EQ(result.TryTake()->value(), 32);

    std::atomic_int destroyed{0};
    std::shared_ptr<NCommon::NJobInternal::JobControl> source;
    auto dropped = SpawnObserved(
            engine,
            AwaitWithLifetime(Pending([] { return 1; }, source), std::make_unique<FrameProbe>(destroyed), resumed),
            owner);
    ASSERT_TRUE(Eventually([&] { return IsWaiting(owner); }));
    dropped = {};
    EXPECT_EQ(destroyed.load(), 0);
    source->Run();
    ASSERT_TRUE(Eventually([&] { return destroyed.load() == 1; }));
    EXPECT_EQ(resumed.load(), 33);
    engine.Stop();
}

TEST(JobAwait, ConcurrentCompletionRegistrationAndCancellationNeverLoseOrDoubleResume) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        NEngine::Engine engine{{.WorkerCount = workers}};
        engine.Start();
        for (int iteration = 0; iteration < 200; ++iteration) {
            std::shared_ptr<NCommon::NJobInternal::JobControl> source;
            std::shared_ptr<NCommon::NJobInternal::JobControl> owner;
            std::atomic_int resumed{0};
            auto job = Pending([] { return 8; }, source);
            std::barrier start{2};
            std::thread completion{[&] {
                start.arrive_and_wait();
                source->Run();
            }};
            start.arrive_and_wait();
            auto result = SpawnObserved(engine, AwaitValue(std::move(job), resumed), owner);
            if (iteration % 2 == 0) {
                result.RequestCancel();
            }
            completion.join();
            ASSERT_TRUE(Eventually([&] { return result.IsReady(); }));
            const auto value = result.TryTake();
            ASSERT_TRUE(value);
            EXPECT_LE(resumed.load(), 1);
            if (value->has_value()) {
                EXPECT_EQ(value->value(), 9);
                EXPECT_EQ(resumed.load(), 1);
            } else {
                EXPECT_EQ(value->error().Code, NCommon::EError::CANCELLED);
            }
        }
        engine.Stop();
    }
}

struct ReleaseWorkerOnExit {
    std::atomic_bool& Released;
    ~ReleaseWorkerOnExit() {
        Released.store(true);
        Released.notify_all();
    }
};

TEST(JobAwait, CancelQueuedContinuationDoesNotResumeAndSubmitFailureRetiresFrame) {
    NEngine::Engine engine{{.WorkerCount = 1}};
    engine.Start();
    std::shared_ptr<NCommon::NJobInternal::JobControl> source;
    std::shared_ptr<NCommon::NJobInternal::JobControl> owner;
    std::atomic_int resumed{0};
    std::atomic_int destroyed{0};
    auto result = SpawnObserved(
            engine,
            AwaitWithLifetime(Pending([] { return 42; }, source), std::make_unique<FrameProbe>(destroyed), resumed),
            owner);
    ASSERT_TRUE(Eventually([&] { return IsWaiting(owner); }));
    std::atomic_bool entered{false};
    std::atomic_bool released{false};
    // The guard releases the worker before Engine destruction even after a fatal assertion.
    const ReleaseWorkerOnExit guard{released};
    auto busy = engine.RunAsync([&] {
        entered.store(true);
        released.wait(false);
    });
    ASSERT_TRUE(Eventually([&] { return entered.load(); }));
    source->Run();
    ASSERT_TRUE(Eventually([&] { return owner->Phase.load() == NCommon::NJobInternal::EJobPhase::RESUME_QUEUED; }));
    result.RequestCancel();
    ASSERT_TRUE(Eventually([&] { return result.IsReady(); }));
    EXPECT_EQ(result.TryTake()->error().Code, NCommon::EError::CANCELLED);
    EXPECT_EQ(destroyed.load(), 1);
    EXPECT_EQ(resumed.load(), 0);
    released.store(true);
    released.notify_all();
    ASSERT_TRUE(Eventually([&] { return busy.IsReady(); }));
    engine.Stop();

    // Drive the pre-publication Submit-failure path without allocating inside terminal cleanup.
    auto failed = NCommon::NJobInternal::JobFactory::Spawn(
            [&owner](const std::shared_ptr<NCommon::NJobInternal::JobControl>& state) { owner = state; },
            AwaitWithLifetime(Pending([] { return 42; }, source), std::make_unique<FrameProbe>(destroyed), resumed));
    owner->Run();
    ASSERT_TRUE(IsWaiting(owner));
    source->Run();
    ASSERT_TRUE(owner->ClaimResume());
    owner->FailResume();
    EXPECT_TRUE(failed.IsReady());
    EXPECT_EQ(failed.TryTake()->error().Code, NCommon::EError::OUT_OF_MEMORY);
    EXPECT_EQ(destroyed.load(), 2);
    EXPECT_EQ(resumed.load(), 0);
}

TEST(JobAwait, CompletionAndSubscriptionLinearizeBeforeDuringAndAfterRegistration) {
    for (int order = 0; order < 3; ++order) {
        for (int iteration = 0; iteration < 100; ++iteration) {
            std::shared_ptr<NCommon::NJobInternal::JobControl> source;
            auto job = Pending([] { return 42; }, source);
            const auto registration = std::make_shared<NCommon::NJobInternal::JobContinuation>();
            bool subscribed = false;
            if (order == 0) {
                source->Run();
                subscribed = source->Subscribe(registration);
            } else if (order == 1) {
                subscribed = source->Subscribe(registration);
                source->Run();
            } else {
                std::barrier start{2};
                std::thread producer{[&] {
                    start.arrive_and_wait();
                    source->Run();
                }};
                start.arrive_and_wait();
                subscribed = source->Subscribe(registration);
                producer.join();
            }
            EXPECT_TRUE(source->IsReady());
            EXPECT_EQ(registration->Ready.load(), subscribed);
            EXPECT_EQ(job.TryTake()->value(), 42);
        }
    }
}

} // namespace
