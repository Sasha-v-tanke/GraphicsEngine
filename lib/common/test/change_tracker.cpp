#include <array>
#include <atomic>
#include <limits>
#include <thread>

#include <GraphicsEngine/lib/common/change_tracker.h>
#include <gtest/gtest.h>

namespace {
using NCommon::ChangeCursor;
using NCommon::ChangeDomainKey;
using NCommon::ChangeTracker;
using NCommon::DirtyRange;
using NCommon::DirtySummary;
constexpr ChangeDomainKey TRACKED_DOMAIN{1, 1, 1};
constexpr ChangeDomainKey OTHER_DOMAIN{1, 2, 1};

void Publish(ChangeTracker& tracker, const DirtySummary& summary) {
    const DirtySummary* pointer = &summary;
    static_cast<void>(tracker.PublishEpoch({&pointer, 1}));
}

TEST(ChangeTracker, IndependentConsumersAndImmutableSnapshot) {
    ChangeTracker tracker;
    tracker.RegisterDomain(TRACKED_DOMAIN, true);
    tracker.RegisterDomain(OTHER_DOMAIN);
    static_cast<void>(tracker.PublishEpoch());
    const auto initial = tracker.CaptureSnapshot();
    const auto first = initial.GetCursor();
    EXPECT_TRUE(initial.CaptureChanges({}, TRACKED_DOMAIN).FullDirty);
    EXPECT_FALSE(initial.CaptureChanges(first, TRACKED_DOMAIN).HasChanges());
    DirtySummary left;
    DirtySummary right;
    std::jthread a([&] { left.MarkEntity(TRACKED_DOMAIN, {2, 7}); });
    std::jthread b([&] { right.MarkRange(TRACKED_DOMAIN, 4, 2); });
    a.join();
    b.join();
    std::array<const DirtySummary*, 2> inputs{&left, &right};
    static_cast<void>(tracker.PublishEpoch(inputs));
    const auto second = tracker.CaptureSnapshot();
    EXPECT_EQ(second.CaptureChanges(first, TRACKED_DOMAIN).Ranges, (std::vector<DirtyRange>{{2, 1}, {4, 2}}));
    EXPECT_FALSE(second.CaptureChanges(first, OTHER_DOMAIN).HasChanges());
    EXPECT_EQ(second.GetEntityRevision(TRACKED_DOMAIN, {2, 7}), second.GetCursor().Epoch);
    EXPECT_EQ(second.GetChunkVersion(TRACKED_DOMAIN, 0), second.GetStorageVersion(TRACKED_DOMAIN));
    EXPECT_EQ(initial.GetStorageVersion(TRACKED_DOMAIN), first.Epoch);
    EXPECT_FALSE(initial.CaptureChanges(first, TRACKED_DOMAIN).HasChanges());
    EXPECT_FALSE(second.CaptureChanges(second.GetCursor(), TRACKED_DOMAIN).HasChanges());
    left.Clear();
    left.MarkRange(TRACKED_DOMAIN, 3, 1);
    Publish(tracker, left);
    EXPECT_EQ(tracker.CaptureChanges(first, TRACKED_DOMAIN).Ranges, (std::vector<DirtyRange>{{2, 4}}));
    EXPECT_EQ(tracker.CaptureChanges(second.GetCursor(), TRACKED_DOMAIN).Ranges, (std::vector<DirtyRange>{{3, 1}}));
}

TEST(ChangeTracker, BoundedHistoryAndSparseMetadataFallback) {
    ChangeTracker tracker({.MaxHistoryEpochs = 2,
                           .MaxHistoryRecords = 6,
                           .MaxChunkVersionsPerDomain = 2,
                           .MaxEntityVersionsPerDomain = 2,
                           .ChunkSize = 1});
    tracker.RegisterDomain(TRACKED_DOMAIN, true);
    tracker.RegisterDomain(OTHER_DOMAIN);
    static_cast<void>(tracker.PublishEpoch());
    const auto slow = tracker.CaptureSnapshot().GetCursor();
    for (std::uint64_t index = 0; index < 100; ++index) {
        DirtySummary dirty;
        dirty.MarkEntity(TRACKED_DOMAIN, {index, 1});
        Publish(tracker, dirty);
        const auto stats = tracker.CaptureSnapshot().GetStatistics();
        EXPECT_LE(stats.HistoryEpochs, 2U);
        EXPECT_LE(stats.HistoryRecords, 6U);
        EXPECT_LE(stats.ChunkVersions, 2U);
        EXPECT_LE(stats.EntityVersions, 2U);
        EXPECT_EQ(stats.Domains, 2U);
    }
    EXPECT_TRUE(tracker.CaptureChanges(slow, TRACKED_DOMAIN).FullDirty);
    EXPECT_FALSE(tracker.CaptureChanges(slow, OTHER_DOMAIN).HasChanges());
}

TEST(ChangeTracker, WrapGenerationAndForeignIdentity) {
    ChangeTracker tracker({.InitialEpoch = {1, std::numeric_limits<std::uint64_t>::max() - 1}});
    tracker.RegisterDomain(TRACKED_DOMAIN);
    static_cast<void>(tracker.PublishEpoch());
    const auto cursor = tracker.CaptureSnapshot().GetCursor();
    static_cast<void>(tracker.PublishEpoch());
    EXPECT_EQ(tracker.CaptureSnapshot().GetCursor().Epoch, (NCommon::WorldEpoch{2, 1}));
    EXPECT_TRUE(tracker.CaptureChanges(cursor, TRACKED_DOMAIN).FullDirty);
    const ChangeDomainKey replacement{1, 1, 2};
    tracker.RegisterDomain(replacement);
    static_cast<void>(tracker.PublishEpoch());
    EXPECT_THROW(static_cast<void>(tracker.CaptureChanges(cursor, TRACKED_DOMAIN)), std::invalid_argument);
    EXPECT_TRUE(tracker.CaptureChanges(cursor, replacement).FullDirty);
    EXPECT_THROW(tracker.RegisterDomain(TRACKED_DOMAIN), std::invalid_argument);
    ChangeTracker foreign;
    EXPECT_THROW(static_cast<void>(tracker.CaptureChanges(foreign.CaptureSnapshot().GetCursor(), replacement)),
                 std::invalid_argument);
    ChangeTracker exhausted(
            {.InitialEpoch = {std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()}});
    EXPECT_THROW(exhausted.PublishEpoch(), std::overflow_error);
}

TEST(ChangeTracker, FailedPublicationRemainsConservativelyDirtyUntilRecovery) {
    ChangeTracker tracker;
    tracker.RegisterDomain(TRACKED_DOMAIN, true);
    static_cast<void>(tracker.PublishEpoch());
    const auto old = tracker.CaptureSnapshot();
    DirtySummary invalid;
    invalid.MarkAll(OTHER_DOMAIN);
    EXPECT_THROW(Publish(tracker, invalid), std::invalid_argument);
    EXPECT_TRUE(tracker.CaptureChanges(old.GetCursor(), TRACKED_DOMAIN).FullDirty);
    EXPECT_FALSE(tracker.CaptureSnapshot().GetStorageVersion(TRACKED_DOMAIN).IsValid());
    EXPECT_FALSE(old.CaptureChanges(old.GetCursor(), TRACKED_DOMAIN).HasChanges());
    static_cast<void>(tracker.PublishEpoch());
    EXPECT_TRUE(tracker.CaptureChanges(old.GetCursor(), TRACKED_DOMAIN).FullDirty);
    EXPECT_FALSE(tracker.CaptureChanges(tracker.CaptureSnapshot().GetCursor(), TRACKED_DOMAIN).HasChanges());
}

TEST(ChangeTracker, RangeOnlyWritesInvalidatePreciseRevisionsAndRejectOverflow) {
    ChangeTracker tracker;
    tracker.RegisterDomain(TRACKED_DOMAIN, true);
    static_cast<void>(tracker.PublishEpoch());
    DirtySummary dirty;
    dirty.MarkEntity(TRACKED_DOMAIN, {5, 1});
    Publish(tracker, dirty);
    EXPECT_TRUE(tracker.CaptureSnapshot().GetEntityRevision(TRACKED_DOMAIN, {5, 1}).has_value());
    dirty.Clear();
    dirty.MarkRange(TRACKED_DOMAIN, 5, 1);
    Publish(tracker, dirty);
    EXPECT_FALSE(tracker.CaptureSnapshot().GetEntityRevision(TRACKED_DOMAIN, {5, 1}).has_value());
    EXPECT_THROW(dirty.MarkRange(TRACKED_DOMAIN, std::numeric_limits<std::uint64_t>::max(), 1), std::invalid_argument);
}

TEST(ChangeTracker, ConcurrentReadersAndInvalidationNeverLoseDirtyState) {
    ChangeTracker tracker;
    tracker.RegisterDomain(TRACKED_DOMAIN);
    static_cast<void>(tracker.PublishEpoch());
    std::atomic_bool finished = false;
    std::jthread reader([&] {
        ChangeCursor cursor;
        while (!finished.load()) {
            const auto snapshot = tracker.CaptureSnapshot();
            const auto changes = snapshot.CaptureChanges(cursor, TRACKED_DOMAIN);
            EXPECT_EQ(changes.Cursor, snapshot.GetCursor());
            cursor = changes.Cursor;
        }
    });
    std::jthread invalidator([&] {
        for (int index = 0; index < 1000; ++index) {
            tracker.InvalidateAll();
        }
    });
    for (int index = 0; index < 1000; ++index) {
        DirtySummary dirty;
        dirty.MarkRange(TRACKED_DOMAIN, 0, 1);
        Publish(tracker, dirty);
    }
    invalidator.join();
    tracker.InvalidateAll();
    const auto snapshot = tracker.CaptureSnapshot();
    EXPECT_TRUE(snapshot.CaptureChanges(snapshot.GetCursor(), TRACKED_DOMAIN).FullDirty);
    finished.store(true);
}
} // namespace
