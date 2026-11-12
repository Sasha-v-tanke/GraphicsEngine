#include <memory>
#include <utility>

#include <graphics/backend/submission_uses.h>
#include <gtest/gtest.h>
#include <resources/resource_manager.h>
#include <tests/common/test_error.h>

namespace {
using NGraphics::NBackend::SubmissionRetirementQueue;
using NGraphics::NBackend::SubmissionUseSet;

struct GpuOwner {
    explicit GpuOwner(int& destroyed)
        : Destroyed(destroyed) {
    }
    ~GpuOwner() {
        ++Destroyed;
    }
    int& Destroyed;
};

TEST(SubmissionUses, LogicalUnloadDeniesAcquisitionButLastSubmissionPinsGpuVersion) {
    int destroyed = 0;
    SubmissionRetirementQueue queue;
    NResources::ResourceManager resources;
    const auto handle = resources.Request<int>({"test", "gpu-version"});
    const auto operation = resources.BeginLoading(handle);
    const auto cpu = std::make_shared<const int>(7);
    auto gpu = std::make_shared<GpuOwner>(destroyed);
    ASSERT_TRUE(resources.TryAdvance(operation,
                                     NResources::EResourceState::LOADING,
                                     NResources::EResourceState::CPU_READY,
                                     cpu));
    ASSERT_TRUE(resources.TryAdvance(operation,
                                     NResources::EResourceState::CPU_READY,
                                     NResources::EResourceState::READY,
                                     cpu,
                                     gpu));
    auto lease = resources.TryAcquire(handle);
    ASSERT_TRUE(lease);
    SubmissionUseSet uses{lease->GetGpuRepresentation()};
    queue.Prepare(10, uses).Commit();
    queue.Prepare(900, uses).Commit();
    uses.clear();
    lease.reset();
    gpu.reset();
    resources.RequestUnload(handle);
    resources.CompleteUnload(handle);
    EXPECT_FALSE(resources.TryAcquire(handle));
    EXPECT_EQ(destroyed, 0);
    queue.TakeCompleted(9).clear();
    EXPECT_EQ(destroyed, 0);
    queue.TakeCompleted(10).clear();
    EXPECT_EQ(destroyed, 0);
    EXPECT_EQ(queue.GetPendingSubmissionCount(), 1U);
    queue.TakeCompleted(899).clear();
    EXPECT_EQ(destroyed, 0);
    queue.TakeCompleted(900).clear();
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(queue.GetPendingUseCount(), 0U);
}

TEST(SubmissionUses, RejectedNativeSubmitRollsBackOnlyItsReservationAndCanRetryValue) {
    int destroyed = 0;
    SubmissionRetirementQueue queue;
    auto gpu = std::make_shared<GpuOwner>(destroyed);
    SubmissionUseSet uses{gpu};
    queue.Prepare(1, uses).Commit();
    {
        auto unaccepted = queue.Prepare(2, uses);
        EXPECT_EQ(queue.GetPendingSubmissionCount(), 2U);
        queue.TakeCompleted(1).clear();
        EXPECT_EQ(queue.GetPendingSubmissionCount(), 1U);
    }
    EXPECT_EQ(queue.GetPendingSubmissionCount(), 0U);
    queue.Prepare(2, uses).Commit();
    gpu.reset();
    uses.clear();
    queue.TakeCompleted(1).clear();
    EXPECT_EQ(destroyed, 0);
    queue.TakeCompleted(2).clear();
    EXPECT_EQ(destroyed, 1);
}

TEST(SubmissionUses, IndependentCompletionOwnersDoNotReleaseEachOthersConsumers) {
    int destroyed = 0;
    SubmissionRetirementQueue first;
    SubmissionRetirementQueue second;
    SubmissionUseSet uses{std::make_shared<GpuOwner>(destroyed)};
    first.Prepare(1, uses).Commit();
    second.Prepare(1, uses).Commit();
    uses.clear();
    first.TakeCompleted(1).clear();
    EXPECT_EQ(destroyed, 0);
    second.TakeCompleted(0).clear();
    EXPECT_EQ(destroyed, 0);
    second.TakeCompleted(1).clear();
    EXPECT_EQ(destroyed, 1);
}

TEST(SubmissionUses, MoveReservationAndShutdownCleanupPreserveOwnership) {
    int destroyed = 0;
    SubmissionRetirementQueue queue;
    SubmissionUseSet uses{std::make_shared<GpuOwner>(destroyed)};
    {
        auto prepared = queue.Prepare(1, uses);
        auto moved = std::move(prepared);
        moved.Commit();
    }
    uses.clear();
    EXPECT_EQ(destroyed, 0);
    auto retired = queue.TakeAfterShutdown();
    EXPECT_EQ(destroyed, 0);
    EXPECT_EQ(queue.GetPendingSubmissionCount(), 0U);
    retired.clear();
    EXPECT_EQ(destroyed, 1);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(queue.Prepare(2, {})); });
}

TEST(SubmissionUses, InvalidUsesAndNonmonotonicValuesAreRejectedBeforeSubmission) {
    SubmissionRetirementQueue queue;
    const SubmissionUseSet invalid{nullptr};
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(queue.Prepare(0, {})); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(queue.Prepare(1, invalid)); });
    queue.Prepare(1, {}).Commit();
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(queue.Prepare(1, {})); });
    queue.TakeCompleted(1).clear();
}
} // namespace
