#include <future>
#include <mutex>

#include <engine/runtime/engine_state_manager.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {

using NEngine::EEngineState;
using NEngine::NRuntime::EngineStateManager;

void Start(EngineStateManager& lifecycle) {
    lifecycle.ValidateStart();
    lifecycle.ValidateNextGeneration();
    lifecycle.AdvanceGeneration();
    lifecycle.MarkRunning();
}

TEST(EngineStateManager, StopObservedBeforeRestartCannotCloseNewGeneration) {
    EngineStateManager lifecycle;
    std::mutex mutex;
    std::unique_lock lock{mutex};
    Start(lifecycle);
    const auto oldGeneration = lifecycle.ObserveGeneration();
    ASSERT_TRUE(lifecycle.BeginStop(lock, oldGeneration));
    EXPECT_EQ(lifecycle.GetState(), EEngineState::STOPPING);
    lifecycle.CompleteStop();
    Start(lifecycle);
    EXPECT_GT(lifecycle.GetGeneration(), oldGeneration);
    EXPECT_FALSE(lifecycle.BeginStop(lock, oldGeneration));
    EXPECT_EQ(lifecycle.GetState(), EEngineState::RUNNING);
    ASSERT_TRUE(lifecycle.BeginStop(lock, lifecycle.ObserveGeneration()));
    lifecycle.CompleteStop();
}

TEST(EngineStateManager, PartialStartRollbackRetiresPublishedGenerationAndPermitsRestart) {
    EngineStateManager lifecycle;
    std::mutex mutex;
    std::unique_lock lock{mutex};
    lifecycle.ValidateStart();
    lifecycle.ValidateNextGeneration();
    lifecycle.AdvanceGeneration();
    const auto failedGeneration = lifecycle.ObserveGeneration();
    lifecycle.RollbackStart();
    EXPECT_EQ(lifecycle.GetState(), EEngineState::STOPPED);
    EXPECT_FALSE(lifecycle.BeginStop(lock, failedGeneration));
    Start(lifecycle);
    EXPECT_GT(lifecycle.GetGeneration(), failedGeneration);
    EXPECT_FALSE(lifecycle.BeginStop(lock, failedGeneration));
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { lifecycle.ValidateStart(); });
    ASSERT_TRUE(lifecycle.BeginStop(lock, lifecycle.ObserveGeneration()));
    lifecycle.CompleteStop();
}

TEST(EngineStateManager, OnlyOneConcurrentStopCallerOwnsCleanup) {
    EngineStateManager lifecycle;
    std::mutex mutex;
    std::unique_lock lock{mutex};
    Start(lifecycle);
    const auto generation = lifecycle.ObserveGeneration();
    ASSERT_TRUE(lifecycle.BeginStop(lock, generation));
    lock.unlock();
    auto follower = std::async(std::launch::async, [&] {
        std::unique_lock followerLock{mutex};
        return lifecycle.BeginStop(followerLock, generation);
    });
    lock.lock();
    lifecycle.CompleteStop();
    lock.unlock();
    EXPECT_FALSE(follower.get());
    lock.lock();
    EXPECT_EQ(lifecycle.GetState(), EEngineState::STOPPED);
    EXPECT_FALSE(lifecycle.BeginStop(lock, generation));
}

} // namespace
