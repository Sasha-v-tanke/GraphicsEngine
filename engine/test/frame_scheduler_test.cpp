#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <thread>

#include <engine/controller/frame_scheduler.h>
#include <engine/engine_config.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {

using NEngine::EngineConfig;
using NEngine::NController::EFrameState;
using NEngine::NController::FrameHandle;
using NEngine::NController::FrameScheduler;
using NTest::ExpectError;

void CompleteFrame(FrameScheduler& scheduler, FrameHandle frame) {
    scheduler.ArmFrame(frame);
    scheduler.BeginUpdate(frame);
    scheduler.EndUpdate(frame);
    scheduler.SignalDraw(frame);
    scheduler.BeginFinalize(frame);
    scheduler.CompleteFrame(frame);
}

void CompleteSignaledFrame(FrameScheduler& scheduler, FrameHandle frame) {
    scheduler.ArmFrame(frame);
    scheduler.BeginUpdate(frame);
    scheduler.EndUpdate(frame);
    scheduler.BeginFinalize(frame);
    scheduler.CompleteFrame(frame);
}

TEST(FrameScheduler, RejectsZeroMaxActiveFrames) {
    const EngineConfig config{
            .MaxActiveFrames = 0,
    };

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { FrameScheduler scheduler{config}; });
}

TEST(FrameScheduler, AcquiresSingleSlot) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const std::optional<FrameHandle> frame = scheduler.TryAcquireFrame();

    ASSERT_TRUE(frame.has_value());

    EXPECT_EQ(frame->GetFrameIndex(), 0U);
    EXPECT_EQ(frame->GetSlotIndex(), 0U);
    EXPECT_EQ(frame->GetGeneration(), 1U);
    EXPECT_EQ(scheduler.GetState(*frame), EFrameState::ACQUIRED);
}

TEST(FrameScheduler, DoesNotReuseActiveSlot) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const std::optional<FrameHandle> frame = scheduler.TryAcquireFrame();

    ASSERT_TRUE(frame.has_value());

    scheduler.SignalDraw(*frame);

    EXPECT_FALSE(scheduler.TryAcquireFrame().has_value());

    CompleteSignaledFrame(scheduler, *frame);

    EXPECT_EQ(scheduler.GetState(*frame), EFrameState::COMPLETE);
    EXPECT_FALSE(scheduler.TryAcquireFrame().has_value());
}

TEST(FrameScheduler, ReusesSlotOnlyAfterRecycle) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle first = *scheduler.TryAcquireFrame();

    CompleteFrame(scheduler, first);
    scheduler.RecycleFrame(first);

    const std::optional<FrameHandle> second = scheduler.TryAcquireFrame();

    ASSERT_TRUE(second.has_value());

    EXPECT_EQ(second->GetFrameIndex(), 1U);
    EXPECT_EQ(second->GetSlotIndex(), 0U);
    EXPECT_EQ(second->GetGeneration(), 2U);
}

TEST(FrameScheduler, SupportsMultipleActiveFrames) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 3,
    }};

    const std::optional<FrameHandle> first = scheduler.TryAcquireFrame();
    ASSERT_TRUE(first.has_value());
    scheduler.SignalDraw(*first);

    const std::optional<FrameHandle> second = scheduler.TryAcquireFrame();
    ASSERT_TRUE(second.has_value());
    scheduler.SignalDraw(*second);

    const std::optional<FrameHandle> third = scheduler.TryAcquireFrame();

    ASSERT_TRUE(third.has_value());
    scheduler.SignalDraw(*third);

    EXPECT_EQ(first->GetSlotIndex(), 0U);
    EXPECT_EQ(second->GetSlotIndex(), 1U);
    EXPECT_EQ(third->GetSlotIndex(), 2U);

    EXPECT_FALSE(scheduler.TryAcquireFrame().has_value());
}

TEST(FrameScheduler, AbortsFrameFromEveryNonFreeState) {
    enum class EAbortPoint {
        ACQUIRED,
        WAITING_UPDATE,
        UPDATING,
        WAITING_DRAW,
        FINALIZE,
        COMPLETE,
    };

    const EAbortPoint abortPoints[] = {
            EAbortPoint::ACQUIRED,
            EAbortPoint::WAITING_UPDATE,
            EAbortPoint::UPDATING,
            EAbortPoint::WAITING_DRAW,
            EAbortPoint::FINALIZE,
            EAbortPoint::COMPLETE,
    };

    for (const EAbortPoint abortPoint: abortPoints) {
        FrameScheduler scheduler{EngineConfig{
                .MaxActiveFrames = 1,
        }};

        const FrameHandle frame = *scheduler.TryAcquireFrame();

        if (abortPoint != EAbortPoint::ACQUIRED) {
            scheduler.ArmFrame(frame);
        }

        if (abortPoint == EAbortPoint::UPDATING || abortPoint == EAbortPoint::WAITING_DRAW ||
            abortPoint == EAbortPoint::FINALIZE || abortPoint == EAbortPoint::COMPLETE) {
            scheduler.BeginUpdate(frame);
        }

        if (abortPoint == EAbortPoint::WAITING_DRAW || abortPoint == EAbortPoint::FINALIZE ||
            abortPoint == EAbortPoint::COMPLETE) {
            scheduler.EndUpdate(frame);
        }

        if (abortPoint == EAbortPoint::FINALIZE || abortPoint == EAbortPoint::COMPLETE) {
            scheduler.SignalDraw(frame);
            scheduler.BeginFinalize(frame);
        }

        if (abortPoint == EAbortPoint::COMPLETE) {
            scheduler.CompleteFrame(frame);
        }

        scheduler.AbortFrame(frame);

        ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(scheduler.GetState(frame)); });

        const std::optional<FrameHandle> nextFrame = scheduler.TryAcquireFrame();

        ASSERT_TRUE(nextFrame.has_value());
        EXPECT_EQ(nextFrame->GetSlotIndex(), frame.GetSlotIndex());
        EXPECT_NE(nextFrame->GetGeneration(), frame.GetGeneration());
    }
}

TEST(FrameScheduler, UsesFrameIndexModuloSlotCount) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 2,
    }};

    for (std::uint64_t frameIndex = 0; frameIndex < 64; ++frameIndex) {
        const std::optional<FrameHandle> frame = scheduler.TryAcquireFrame();

        ASSERT_TRUE(frame.has_value());

        EXPECT_EQ(frame->GetFrameIndex(), frameIndex);
        EXPECT_EQ(frame->GetSlotIndex(), frameIndex % 2);

        CompleteFrame(scheduler, *frame);
        scheduler.RecycleFrame(*frame);
    }
}

TEST(FrameScheduler, IncrementsGenerationOnSlotReuse) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 2,
    }};

    const FrameHandle first = *scheduler.TryAcquireFrame();
    scheduler.SignalDraw(first);

    const FrameHandle second = *scheduler.TryAcquireFrame();
    scheduler.SignalDraw(second);

    EXPECT_EQ(first.GetGeneration(), 1U);
    EXPECT_EQ(second.GetGeneration(), 1U);

    CompleteSignaledFrame(scheduler, first);
    scheduler.RecycleFrame(first);

    CompleteSignaledFrame(scheduler, second);
    scheduler.RecycleFrame(second);

    const FrameHandle third = *scheduler.TryAcquireFrame();
    scheduler.SignalDraw(third);

    const FrameHandle fourth = *scheduler.TryAcquireFrame();

    EXPECT_EQ(third.GetSlotIndex(), 0U);
    EXPECT_EQ(third.GetGeneration(), 2U);

    EXPECT_EQ(fourth.GetSlotIndex(), 1U);
    EXPECT_EQ(fourth.GetGeneration(), 2U);
}

TEST(FrameScheduler, RejectsStaleHandleAfterRecycle) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle frame = *scheduler.TryAcquireFrame();

    CompleteFrame(scheduler, frame);
    scheduler.RecycleFrame(frame);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(scheduler.GetState(frame)); });
}

TEST(FrameScheduler, RejectsStaleHandleAfterSlotReuse) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle first = *scheduler.TryAcquireFrame();

    CompleteFrame(scheduler, first);
    scheduler.RecycleFrame(first);

    const FrameHandle second = *scheduler.TryAcquireFrame();

    EXPECT_EQ(first.GetSlotIndex(), second.GetSlotIndex());
    EXPECT_NE(first.GetGeneration(), second.GetGeneration());

    EXPECT_THROW(scheduler.ArmFrame(first), NCommon::Exception);
}

TEST(FrameScheduler, RejectsForeignHandle) {
    FrameScheduler firstScheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    FrameScheduler secondScheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle firstFrame = *firstScheduler.TryAcquireFrame();
    const FrameHandle secondFrame = *secondScheduler.TryAcquireFrame();

    EXPECT_NE(firstFrame, secondFrame);

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(secondScheduler.GetState(firstFrame)); });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { secondScheduler.ArmFrame(firstFrame); });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { secondScheduler.RecycleFrame(firstFrame); });

    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { static_cast<void>(secondScheduler.GetMemoryResource(firstFrame)); });
}

TEST(FrameScheduler, ReusesSlotOwnedMemoryResourceAcrossGenerations) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle first = *scheduler.TryAcquireFrame();

    std::pmr::memory_resource* firstResource = &scheduler.GetMemoryResource(first);

    CompleteFrame(scheduler, first);
    scheduler.RecycleFrame(first);

    const FrameHandle second = *scheduler.TryAcquireFrame();

    std::pmr::memory_resource* secondResource = &scheduler.GetMemoryResource(second);

    EXPECT_EQ(first.GetSlotIndex(), second.GetSlotIndex());
    EXPECT_NE(first.GetGeneration(), second.GetGeneration());
    EXPECT_EQ(firstResource, secondResource);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(scheduler.GetMemoryResource(first)); });
}

TEST(FrameScheduler, FollowsRequiredStateLifecycle) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle frame = *scheduler.TryAcquireFrame();

    EXPECT_EQ(scheduler.GetState(frame), EFrameState::ACQUIRED);

    scheduler.ArmFrame(frame);
    EXPECT_EQ(scheduler.GetState(frame), EFrameState::WAITING_UPDATE);

    scheduler.BeginUpdate(frame);
    EXPECT_EQ(scheduler.GetState(frame), EFrameState::UPDATING);

    scheduler.EndUpdate(frame);
    EXPECT_EQ(scheduler.GetState(frame), EFrameState::WAITING_DRAW);

    scheduler.SignalDraw(frame);

    scheduler.BeginFinalize(frame);
    EXPECT_EQ(scheduler.GetState(frame), EFrameState::FINALIZE);

    scheduler.CompleteFrame(frame);
    EXPECT_EQ(scheduler.GetState(frame), EFrameState::COMPLETE);

    scheduler.RecycleFrame(frame);

    const std::optional<FrameHandle> nextFrame = scheduler.TryAcquireFrame();

    ASSERT_TRUE(nextFrame.has_value());
    EXPECT_NE(nextFrame->GetGeneration(), frame.GetGeneration());
}

TEST(FrameScheduler, RejectsHandleFromDestroyedSchedulerReusedAtSameAddress) {
    std::optional<FrameScheduler> scheduler;

    scheduler.emplace(EngineConfig{
            .MaxActiveFrames = 1,
    });

    const FrameScheduler* firstAddress = &*scheduler;
    const FrameHandle staleHandle = *scheduler->TryAcquireFrame();

    scheduler.reset();

    scheduler.emplace(EngineConfig{
            .MaxActiveFrames = 1,
    });

    EXPECT_EQ(&*scheduler, firstAddress);

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(scheduler->GetState(staleHandle)); });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { scheduler->ArmFrame(staleHandle); });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { scheduler->RecycleFrame(staleHandle); });

    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { static_cast<void>(scheduler->GetMemoryResource(staleHandle)); });
}

TEST(FrameScheduler, RejectsIllegalStateTransitions) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle frame = *scheduler.TryAcquireFrame();

    ExpectError(NCommon::EError::INVALID_STATE, [&] { scheduler.BeginUpdate(frame); });

    scheduler.ArmFrame(frame);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { scheduler.ArmFrame(frame); });

    scheduler.BeginUpdate(frame);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { scheduler.BeginFinalize(frame); });

    scheduler.EndUpdate(frame);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { scheduler.CompleteFrame(frame); });

    ExpectError(NCommon::EError::INVALID_STATE, [&] { scheduler.BeginFinalize(frame); });

    scheduler.SignalDraw(frame);

    scheduler.BeginFinalize(frame);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { scheduler.RecycleFrame(frame); });
}

TEST(FrameScheduler, RejectsInvalidCheckpointOrder) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 2,
    }};

    ExpectError(NCommon::EError::INVALID_STATE, [&] { scheduler.SignalDraw(FrameHandle{}); });

    const FrameHandle frame = *scheduler.TryAcquireFrame();

    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(scheduler.TryAcquireFrame()); });

    scheduler.SignalDraw(frame);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { scheduler.SignalDraw(frame); });

    scheduler.ArmFrame(frame);
    scheduler.BeginUpdate(frame);
    scheduler.EndUpdate(frame);
    scheduler.BeginFinalize(frame);
    scheduler.CompleteFrame(frame);
}

TEST(FrameScheduler, RejectsStaleDrawSignalAfterSlotReuse) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle first = *scheduler.TryAcquireFrame();

    CompleteFrame(scheduler, first);
    scheduler.RecycleFrame(first);

    const FrameHandle second = *scheduler.TryAcquireFrame();

    ASSERT_EQ(first.GetFrameSlotIndex(), second.GetFrameSlotIndex());
    ASSERT_NE(first.GetGeneration(), second.GetGeneration());

    ExpectError(NCommon::EError::INVALID_STATE, [&] { scheduler.SignalDraw(first); });

    scheduler.SignalDraw(second);
}

TEST(FrameScheduler, TracksUpdateDeltaTimeFromSteadyClock) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 2,
    }};

    const FrameHandle first = *scheduler.TryAcquireFrame();

    EXPECT_EQ(first.GetApplicationFrameIndex(), 0U);
    EXPECT_EQ(first.GetSimulationIndex(), 0U);
    EXPECT_EQ(first.GetFrameSlotIndex(), 0U);
    EXPECT_EQ(scheduler.GetDeltaTime(first), FrameScheduler::Duration::zero());

    scheduler.SignalDraw(first);

    std::this_thread::sleep_for(std::chrono::milliseconds{1});

    const FrameHandle second = *scheduler.TryAcquireFrame();

    EXPECT_EQ(second.GetApplicationFrameIndex(), 1U);
    EXPECT_EQ(second.GetSimulationIndex(), 1U);
    EXPECT_EQ(second.GetFrameSlotIndex(), 1U);
    EXPECT_GT(scheduler.GetDeltaTime(second), FrameScheduler::Duration::zero());

    scheduler.ArmFrame(second);
    scheduler.BeginUpdate(second);

    scheduler.ArmFrame(first);
    scheduler.BeginUpdate(first);

    EXPECT_EQ(scheduler.GetDeltaTime(first), FrameScheduler::Duration::zero());
    EXPECT_GT(scheduler.GetDeltaTime(second), FrameScheduler::Duration::zero());
}

TEST(FrameScheduler, RejectsDefaultHandleAsInvalidArgument) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle frame;

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(scheduler.GetState(frame)); });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { scheduler.ArmFrame(frame); });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(scheduler.GetMemoryResource(frame)); });
}

TEST(FrameScheduler, KeepsNextMappedSlotAsBackpressureBoundary) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 2,
    }};

    const FrameHandle first = *scheduler.TryAcquireFrame();
    scheduler.SignalDraw(first);

    const FrameHandle second = *scheduler.TryAcquireFrame();
    scheduler.SignalDraw(second);

    CompleteSignaledFrame(scheduler, second);
    scheduler.RecycleFrame(second);

    EXPECT_FALSE(scheduler.TryAcquireFrame().has_value());

    CompleteSignaledFrame(scheduler, first);
    scheduler.RecycleFrame(first);

    const std::optional<FrameHandle> third = scheduler.TryAcquireFrame();

    ASSERT_TRUE(third.has_value());

    EXPECT_EQ(third->GetFrameIndex(), 2U);
    EXPECT_EQ(third->GetSlotIndex(), 0U);
}

TEST(FrameScheduler, OwnsIndependentFrameMemoryResources) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 2,
    }};

    const FrameHandle first = *scheduler.TryAcquireFrame();
    scheduler.SignalDraw(first);

    const FrameHandle second = *scheduler.TryAcquireFrame();

    std::pmr::memory_resource& firstResource = scheduler.GetMemoryResource(first);

    std::pmr::memory_resource& secondResource = scheduler.GetMemoryResource(second);

    EXPECT_NE(&firstResource, &secondResource);

    EXPECT_EQ(&firstResource, &scheduler.GetMemoryResource(first));
}

TEST(FrameScheduler, RejectsMemoryAccessThroughStaleHandle) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle frame = *scheduler.TryAcquireFrame();

    CompleteFrame(scheduler, frame);
    scheduler.RecycleFrame(frame);

    EXPECT_THROW(static_cast<void>(scheduler.GetMemoryResource(frame)), NCommon::Exception);
}

TEST(FrameScheduler, ReportsConfiguredSlotCount) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 4,
    }};

    EXPECT_EQ(scheduler.GetMaxActiveFrames(), 4U);
}

} // namespace
