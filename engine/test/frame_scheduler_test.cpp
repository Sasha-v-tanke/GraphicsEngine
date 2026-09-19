#include <cstddef>
#include <memory_resource>

#include <gtest/gtest.h>
#include <engine/engine_config.h>
#include <engine/internal/frame_scheduler.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace {

using NEngine::EngineConfig;
using NEngine::NInternal::EFrameState;
using NEngine::NInternal::FrameHandle;
using NEngine::NInternal::FrameScheduler;

void CompleteFrame(FrameScheduler& scheduler, FrameHandle frame) {
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

    try {
        FrameScheduler scheduler{config};
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(
                exception.code(),
                NCommon::make_error_code(NCommon::EError::INVALID_ARGUMENT));

        return;
    }

    FAIL() << "FrameScheduler accepted MaxActiveFrames == 0";
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

    EXPECT_FALSE(scheduler.TryAcquireFrame().has_value());

    CompleteFrame(scheduler, *frame);

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
    const std::optional<FrameHandle> second = scheduler.TryAcquireFrame();
    const std::optional<FrameHandle> third = scheduler.TryAcquireFrame();

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());

    EXPECT_EQ(first->GetSlotIndex(), 0U);
    EXPECT_EQ(second->GetSlotIndex(), 1U);
    EXPECT_EQ(third->GetSlotIndex(), 2U);

    EXPECT_FALSE(scheduler.TryAcquireFrame().has_value());
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
    const FrameHandle second = *scheduler.TryAcquireFrame();

    EXPECT_EQ(first.GetGeneration(), 1U);
    EXPECT_EQ(second.GetGeneration(), 1U);

    CompleteFrame(scheduler, first);
    scheduler.RecycleFrame(first);

    CompleteFrame(scheduler, second);
    scheduler.RecycleFrame(second);

    const FrameHandle third = *scheduler.TryAcquireFrame();
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

    EXPECT_THROW(
            static_cast<void>(scheduler.GetState(frame)),
            NCommon::Exception);
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

    EXPECT_THROW(
            scheduler.ArmFrame(first),
            NCommon::Exception);
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

    scheduler.BeginFinalize(frame);
    EXPECT_EQ(scheduler.GetState(frame), EFrameState::FINALIZE);

    scheduler.CompleteFrame(frame);
    EXPECT_EQ(scheduler.GetState(frame), EFrameState::COMPLETE);

    scheduler.RecycleFrame(frame);

    EXPECT_FALSE(scheduler.TryAcquireFrame()->GetGeneration() == frame.GetGeneration());
}

TEST(FrameScheduler, RejectsIllegalStateTransitions) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle frame = *scheduler.TryAcquireFrame();

    EXPECT_THROW(
            scheduler.BeginUpdate(frame),
            NCommon::Exception);

    scheduler.ArmFrame(frame);

    EXPECT_THROW(
            scheduler.ArmFrame(frame),
            NCommon::Exception);

    scheduler.BeginUpdate(frame);

    EXPECT_THROW(
            scheduler.BeginFinalize(frame),
            NCommon::Exception);

    scheduler.EndUpdate(frame);

    EXPECT_THROW(
            scheduler.CompleteFrame(frame),
            NCommon::Exception);

    scheduler.BeginFinalize(frame);

    EXPECT_THROW(
            scheduler.RecycleFrame(frame),
            NCommon::Exception);
}

TEST(FrameScheduler, KeepsNextMappedSlotAsBackpressureBoundary) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 2,
    }};

    const FrameHandle first = *scheduler.TryAcquireFrame();
    const FrameHandle second = *scheduler.TryAcquireFrame();

    CompleteFrame(scheduler, second);
    scheduler.RecycleFrame(second);

    EXPECT_FALSE(scheduler.TryAcquireFrame().has_value());

    CompleteFrame(scheduler, first);
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
    const FrameHandle second = *scheduler.TryAcquireFrame();

    std::pmr::memory_resource& firstResource =
            scheduler.GetMemoryResource(first);

    std::pmr::memory_resource& secondResource =
            scheduler.GetMemoryResource(second);

    EXPECT_NE(&firstResource, &secondResource);

    EXPECT_EQ(
            &firstResource,
            &scheduler.GetMemoryResource(first));
}

TEST(FrameScheduler, RejectsMemoryAccessThroughStaleHandle) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle frame = *scheduler.TryAcquireFrame();

    CompleteFrame(scheduler, frame);
    scheduler.RecycleFrame(frame);

    EXPECT_THROW(
            static_cast<void>(scheduler.GetMemoryResource(frame)),
            NCommon::Exception);
}

TEST(FrameScheduler, ReportsConfiguredSlotCount) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 4,
    }};

    EXPECT_EQ(scheduler.GetMaxActiveFrames(), 4U);
}

} // namespace
