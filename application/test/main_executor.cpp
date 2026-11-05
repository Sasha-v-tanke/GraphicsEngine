#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include <application/runtime/main_executor.h>
#include <gtest/gtest.h>
#include <lib/thread/task/task_system.h>
#include <tests/common/test_error.h>

namespace {

using NApplication::NRuntime::EMainCommandState;
using NApplication::NRuntime::MainCompletion;
using NApplication::NRuntime::MainExecutor;
using namespace std::chrono_literals;
static_assert(!std::is_copy_constructible_v<MainExecutor::Command>);
static_assert(std::is_move_constructible_v<MainExecutor::Command>);
static_assert(!std::is_constructible_v<MainExecutor::Command, decltype([] { return 1; })>);

TEST(MainExecutor, FifoMoveOnlyFailureAndBoundedDispatch) {
    MainExecutor executor;
    std::atomic<int> wakes{0};
    const auto generation =
            executor.Start({&wakes, [](void* value) noexcept { ++*static_cast<std::atomic<int>*>(value); }});
    std::vector<int> order;
    auto first = executor.Post(generation, [value = std::make_unique<int>(1), &order] { order.push_back(*value); });
    auto failed = executor.Post(generation, [] { throw std::runtime_error{"command failure"}; });
    auto last = executor.Post(generation, [&order] { order.push_back(3); });
    EXPECT_EQ(wakes.load(), 3);
    EXPECT_EQ(first.GetState(), EMainCommandState::QUEUED);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)first.Wait(); });
    EXPECT_EQ(executor.Drain(2), 2);
    EXPECT_EQ(first.Wait(), EMainCommandState::COMPLETED);
    EXPECT_EQ(failed.Wait(), EMainCommandState::FAILED);
    ASSERT_NE(failed.GetFailure(), nullptr);
    EXPECT_THROW(std::rethrow_exception(failed.GetFailure()), std::runtime_error);
    EXPECT_EQ(last.GetState(), EMainCommandState::QUEUED);
    EXPECT_EQ(executor.Drain(), 1);
    EXPECT_EQ(last.Wait(), EMainCommandState::COMPLETED);
    EXPECT_EQ(order, (std::vector<int>{1, 3}));
}

TEST(MainExecutor, ConcurrentProducersHaveNoLossAndPreserveEachProducerOrder) {
    MainExecutor executor;
    const auto generation = executor.Start();
    constexpr std::size_t producers = 8;
    constexpr int commands = 100;
    const std::thread::id main = std::this_thread::get_id();
    std::vector<int> next(producers);
    std::vector<std::vector<MainCompletion>> completions(producers);
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (std::size_t producer = 0; producer < producers; ++producer) {
        threads.emplace_back([&, producer] {
            for (int index = 0; index < commands; ++index) {
                completions[producer].push_back(executor.Post(generation, [&, producer, index] {
                    EXPECT_EQ(std::this_thread::get_id(), main);
                    EXPECT_EQ(next[producer]++, index);
                }));
            }
        });
    }
    for (std::thread& thread: threads) {
        thread.join();
    }
    EXPECT_EQ(executor.GetPendingCount(), producers * commands);
    while (executor.Drain() != 0) {
    }
    for (std::size_t producer = 0; producer < producers; ++producer) {
        EXPECT_EQ(next[producer], commands);
        for (const MainCompletion& completion: completions[producer]) {
            EXPECT_EQ(completion.Wait(), EMainCommandState::COMPLETED);
        }
    }
}

TEST(MainExecutor, RejectsWrongConsumerAndReentrancyButAllowsPostingInsideCallback) {
    MainExecutor executor;
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)executor.Post(0, [] {}); });
    const auto generation = executor.Start();
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { executor.Start(); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)executor.Post(generation, {}); });
    auto worker = std::async(std::launch::async, [&] {
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { executor.Drain(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { executor.Start(); });
    });
    worker.get();
    int ran = 0;
    auto root = executor.Post(generation, [&] {
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { executor.Drain(); });
        (void)executor.Post(generation, [&] { ++ran; });
    });
    EXPECT_EQ(executor.Drain(1), 1);
    EXPECT_EQ(root.Wait(), EMainCommandState::COMPLETED);
    EXPECT_EQ(ran, 0);
    EXPECT_EQ(executor.Drain(), 1);
    EXPECT_EQ(ran, 1);
}

TEST(MainExecutor, StopWakesAllWaitersAndRejectsOldRunGeneration) {
    MainExecutor executor;
    const auto old = executor.Start();
    auto skipped = executor.Post(old, [] { ADD_FAILURE() << "Stopped command ran"; });
    std::vector<std::future<EMainCommandState>> waiters;
    waiters.reserve(8);
    for (int index = 0; index < 8; ++index) {
        waiters.push_back(std::async(std::launch::async, [skipped] { return skipped.Wait(); }));
    }
    executor.Stop();
    for (auto& waiter: waiters) {
        ASSERT_EQ(waiter.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(waiter.get(), EMainCommandState::SKIPPED);
    }
    auto afterStop = executor.Post(old, [] { ADD_FAILURE(); });
    EXPECT_EQ(afterStop.Wait(), EMainCommandState::SKIPPED);
    const auto current = executor.Start();
    EXPECT_GT(current, old);
    auto stale = executor.Post(old, [] { ADD_FAILURE(); });
    EXPECT_EQ(stale.Wait(), EMainCommandState::SKIPPED);
    auto accepted = executor.Post(current, [] {});
    EXPECT_EQ(executor.Drain(), 1);
    EXPECT_EQ(accepted.Wait(), EMainCommandState::COMPLETED);
}

TEST(MainExecutor, ConcurrentStopAndPostAlwaysPublishTerminalCompletion) {
    for (int iteration = 0; iteration < 20; ++iteration) {
        MainExecutor executor;
        const auto generation = executor.Start();
        std::barrier start{9};
        std::vector<std::vector<MainCompletion>> completions(8);
        std::vector<std::thread> threads;
        threads.reserve(8);
        for (std::size_t producer = 0; producer < 8; ++producer) {
            threads.emplace_back([&, producer] {
                start.arrive_and_wait();
                for (int index = 0; index < 50; ++index) {
                    completions[producer].push_back(executor.Post(generation, [] { ADD_FAILURE(); }));
                }
            });
        }
        start.arrive_and_wait();
        executor.Stop();
        for (std::thread& thread: threads) {
            thread.join();
        }
        for (const auto& producer: completions) {
            for (const MainCompletion& completion: producer) {
                EXPECT_EQ(completion.Wait(), EMainCommandState::SKIPPED);
            }
        }
        EXPECT_EQ(executor.GetPendingCount(), 0);
    }
}

TEST(MainExecutor, StopSkipsPendingButDoesNotPreemptRunningCommand) {
    MainExecutor executor;
    const auto generation = executor.Start();
    std::promise<void> entered;
    auto started = entered.get_future();
    std::promise<void> release;
    const auto released = release.get_future().share();
    auto running = executor.Post(generation, [&] {
        entered.set_value();
        EXPECT_EQ(released.wait_for(2s), std::future_status::ready);
    });
    auto pending = executor.Post(generation, [] { ADD_FAILURE(); });
    std::thread stopper{[&] {
        EXPECT_EQ(started.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(running.GetState(), EMainCommandState::RUNNING);
        executor.Stop();
        EXPECT_EQ(running.GetState(), EMainCommandState::RUNNING);
        EXPECT_EQ(pending.Wait(), EMainCommandState::SKIPPED);
        release.set_value();
    }};
    EXPECT_EQ(executor.Drain(), 1);
    stopper.join();
    EXPECT_EQ(running.Wait(), EMainCommandState::COMPLETED);
}

TEST(MainExecutor, CompletionSurvivesExecutorAndPayloadReleaseDoesNotHoldQueueLock) {
    std::unique_ptr<MainCompletion> completion;
    {
        MainExecutor executor;
        const auto generation = executor.Start();
        struct Capture {
            MainExecutor& Executor;
            ~Capture() {
                EXPECT_EQ(Executor.GetPendingCount(), 0);
                NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { Executor.Start(); });
            }
        };
        auto payload = std::make_unique<Capture>(executor);
        completion = std::make_unique<MainCompletion>(executor.Post(generation, [payload = std::move(payload)] {}));
        executor.Stop();
    }
    EXPECT_EQ(completion->Wait(), EMainCommandState::SKIPPED);
}

TEST(MainExecutor, SelfReplenishingQueueStillYieldsAtBatchBoundary) {
    MainExecutor executor;
    const auto generation = executor.Start();
    int count = 0;
    std::function<void()> replenish;
    replenish = [&] {
        ++count;
        (void)executor.Post(generation, replenish);
    };
    (void)executor.Post(generation, replenish);
    EXPECT_EQ(executor.Drain(), MainExecutor::DISPATCH_BATCH);
    EXPECT_EQ(count, MainExecutor::DISPATCH_BATCH);
    EXPECT_EQ(executor.GetPendingCount(), 1);
    executor.Stop();
}

} // namespace

namespace {
TEST(MainExecutorPriority, OldestFrameThenFifoAndBoundedUnframedService) {
    MainExecutor executor;
    const auto generation = executor.Start();
    std::vector<int> order;
    auto post = [&](std::optional<NCommon::FrameOrigin> origin, int value) {
        const NCommon::FrameOriginScope scope{origin};
        return executor.Post(generation, [&, value] { order.push_back(value); });
    };
    auto newer = post(NCommon::FrameOrigin{1, 100}, 100);
    auto first = post(NCommon::FrameOrigin{1, 99}, 1);
    auto second = post(NCommon::FrameOrigin{1, 99}, 2);
    executor.Drain();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 100}));
    order.clear();
    auto background = post(std::nullopt, -1);
    for (int index = 0; index < 32; ++index) {
        (void)post(NCommon::FrameOrigin{1, 101}, index);
    }
    for (std::size_t index = 0; index <= MainExecutor::FRAME_BURST; ++index) {
        executor.Drain(1);
    }
    EXPECT_EQ(background.GetState(), EMainCommandState::COMPLETED);
    EXPECT_LE(std::find(order.begin(), order.end(), -1) - order.begin(), MainExecutor::FRAME_BURST);
    EXPECT_GE(executor.GetDiagnostics().PeakDepth, 33);
    EXPECT_GT(executor.GetDiagnostics().MaxQueueAge.count(), 0);
    executor.Stop();
    executor.Start();
    EXPECT_EQ(executor.GetDiagnostics().Dispatched, 0);
    EXPECT_EQ(executor.Post(generation, [] {}).GetState(), EMainCommandState::SKIPPED);
}
} // namespace

namespace {
TEST(MainExecutorPriority, NestedSpawnKeepsOriginAndIndependentWorkerDoesNotInheritIt) {
    NCommon::TaskSystem tasks{1};
    const NCommon::FrameOrigin origin{3, 100};
    NCommon::TaskHandle child;
    {
        const NCommon::FrameOriginScope scope{origin};
        const auto root = tasks.Submit([&](NCommon::TaskContext& context) {
            EXPECT_EQ(NCommon::TaskContext::GetFrameOrigin(), origin);
            child = context.Spawn(
                    [&](NCommon::TaskContext&) { EXPECT_EQ(NCommon::TaskContext::GetFrameOrigin(), origin); });
        });
        tasks.Wait(root);
    }
    tasks.Wait(child);
    const auto independent =
            tasks.Submit([](NCommon::TaskContext&) { EXPECT_FALSE(NCommon::TaskContext::GetFrameOrigin()); });
    tasks.Wait(independent);
}
} // namespace

namespace {
TEST(MainExecutorPriority, MeasuresSynchronousProducerBlocking) {
    MainExecutor executor;
    executor.Start();
    std::jthread producer{[&] { executor.Invoke([] {}); }};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (executor.GetPendingCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(executor.GetPendingCount(), 1);
    executor.Drain();
    executor.Stop();
    producer.join();
    EXPECT_GT(executor.GetDiagnostics().BlockedTime.count(), 0);
}
} // namespace
