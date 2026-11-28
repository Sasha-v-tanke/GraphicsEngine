#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include <GraphicsEngine/math/transform.h>
#include <GraphicsEngine/scene/dynamic_aabb_tree.h>
#include <GraphicsEngine/scene/scene_manager.h>
#include <gtest/gtest.h>
#include <scene/test/bvh_fixture.h>
#include <tests/common/test_error.h>

namespace {

using NScene::DynamicAabbTree;
using NScene::SpatialAabb;
using NScene::SpatialIdentity;
using NScene::SpatialRecord;

SpatialAabb Box(double x, double y = 0, double z = 0) {
    return {{x, y, z}, {x + 1, y + 1, z + 1}};
}

bool Overlaps(SpatialAabb a, SpatialAabb b) {
    return a.Min.X <= b.Max.X && b.Min.X <= a.Max.X && a.Min.Y <= b.Max.Y && b.Min.Y <= a.Max.Y &&
           a.Min.Z <= b.Max.Z && b.Min.Z <= a.Max.Z;
}

NScene::SpatialFrustum Frustum(SpatialAabb box) {
    return {{{{{1, 0, 0}, -box.Min.X},
              {{-1, 0, 0}, box.Max.X},
              {{0, 1, 0}, -box.Min.Y},
              {{0, -1, 0}, box.Max.Y},
              {{0, 0, 1}, -box.Min.Z},
              {{0, 0, -1}, box.Max.Z}}}};
}

class DynamicAabbTreeTest: public testing::Test {
protected:
    NScene::SceneManager Scenes;
    NScene::SceneHandle Scene = Scenes.CreateScene();

    SpatialRecord Record(std::uint32_t slot, SpatialAabb bounds) const {
        auto record = NBvhTest::Record(Scene, slot, bounds);
        record.Mobility = NScene::ESpatialMobility::DYNAMIC;
        return record;
    }

    std::vector<SpatialRecord> Grid(std::uint32_t count) const {
        std::vector<SpatialRecord> records;
        records.reserve(count);
        for (std::uint32_t slot = 0; slot < count; ++slot) {
            records.push_back(Record(slot, Box(static_cast<double>(slot) * 4)));
        }
        return records;
    }
};

TEST_F(DynamicAabbTreeTest, TightBoundsUpdateWithoutReinsertionAndTeleportReinserts) {
    auto records = Grid(2);
    const auto first = DynamicAabbTree::Build(Scene, 1, records);
    ASSERT_EQ(first.GetStatistics().Inserted, 2U);
    EXPECT_EQ(first.GetStatistics().AllocatedNodes, 3U);
    records[0].Bounds->Aabb = Box(0.25);
    ++records[0].SourceRevision;
    const auto second = first.Refresh(2, records);
    EXPECT_EQ(second.GetStatistics().UpdatedInFatBounds, 1U);
    EXPECT_EQ(second.GetStatistics().Reinserted, 0U);
    EXPECT_EQ(second.GetStatistics().Rotations, 0U);
    const SpatialAabb oldOnly{{0, 0, 0}, {0.1, 1, 1}};
    EXPECT_EQ(first.QueryOverlap(oldOnly).Candidates, std::vector{records[0].Identity});
    EXPECT_TRUE(second.QueryOverlap(oldOnly).Candidates.empty());
    EXPECT_EQ(second.QueryOverlap(Box(1.1)).Candidates, std::vector{records[0].Identity});
    records[0].Bounds->Aabb = Box(1000);
    const auto third = second.Refresh(3, records);
    EXPECT_EQ(third.GetStatistics().Reinserted, 1U);
    EXPECT_EQ(third.GetStatistics().AllocatedNodes, 3U);
    EXPECT_TRUE(third.QueryOverlap(Box(0)).Candidates.empty());
    EXPECT_EQ(third.QueryOverlap(Box(1000)).Candidates, std::vector{records[0].Identity});
    EXPECT_EQ(second.QueryOverlap(Box(0)).Version, 2U);
    EXPECT_EQ(second.QueryOverlap(Box(0)).Candidates, std::vector{records[0].Identity});
}

TEST_F(DynamicAabbTreeTest, RecycledSlotsClassificationAndEmptySnapshot) {
    auto records = Grid(3);
    const auto first = DynamicAabbTree::Build(Scene, 1, records);
    const auto oldIdentity = records[0].Identity;
    ++records[0].Identity.Entity.Identity.Generation;
    records[1].Mobility = NScene::ESpatialMobility::STATIC;
    records[2].Validity = NScene::ESpatialValidity::DISABLED;
    const auto second = first.Refresh(2, records);
    EXPECT_EQ(second.GetStatistics().Inserted, 1U);
    EXPECT_EQ(second.GetStatistics().Removed, 3U);
    EXPECT_EQ(second.GetStatistics().SkippedRecords, 2U);
    EXPECT_EQ(second.GetStatistics().IndexedRecords, 1U);
    EXPECT_EQ(first.QueryOverlap(Box(0)).Candidates, std::vector{oldIdentity});
    EXPECT_EQ(second.QueryOverlap(Box(0)).Candidates, std::vector{records[0].Identity});
    EXPECT_TRUE(second.QueryOverlap(Box(4)).Candidates.empty());
    const auto empty = second.Refresh(3, {});
    EXPECT_EQ(empty.GetStatistics().MaxDepth, 0U);
    EXPECT_TRUE(empty.QueryRay({}).Candidates.empty());
    const auto next = empty.Refresh(4, records);
    EXPECT_EQ(next.GetStatistics().AllocatedNodes, first.GetStatistics().AllocatedNodes);
    EXPECT_EQ(next.QueryOverlap(Box(0)).Candidates, std::vector{records[0].Identity});
}

TEST_F(DynamicAabbTreeTest, InvalidRefreshDoesNotChangeRetainedSnapshot) {
    const auto records = Grid(2);
    const auto first = DynamicAabbTree::Build(Scene, 1, records, {.MaxRecords = 3});
    const auto reject = [&](const std::vector<SpatialRecord>& input) {
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)first.Refresh(2, input); });
    };
    auto bad = records;
    bad[1].Identity = bad[0].Identity;
    ++bad[1].Identity.Entity.Identity.Generation;
    reject(bad);
    bad = records;
    ++bad[0].Identity.Entity.WorldId;
    reject(bad);
    bad = records;
    bad[0].Identity.Scene = Scenes.CreateScene();
    reject(bad);
    bad = records;
    bad[0].Bounds.reset();
    reject(bad);
    bad = records;
    bad[0].SourceRevision = 0;
    reject(bad);
    bad = records;
    bad[0].Bounds->Aabb.Max.X = std::numeric_limits<double>::quiet_NaN();
    reject(bad);
    reject(Grid(4));
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)first.Refresh(1, records); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { (void)DynamicAabbTree::Build(Scene, 0, records); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { (void)DynamicAabbTree::Build(Scene, 1, records, {.FatMargin = -1}); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { (void)first.QueryRay({.Direction = {}}); });
    EXPECT_EQ(first.GetVersion(), 1U);
    EXPECT_EQ(first.QueryOverlap(Box(0)).Candidates, std::vector{records[0].Identity});
}

TEST_F(DynamicAabbTreeTest, HighChurnMaintainsBalancedTreeAndMatchesIndependentOracle) {
    auto records = Grid(512);
    auto tree = DynamicAabbTree::Build(Scene, 1, records);
    EXPECT_GT(tree.GetStatistics().Rotations, 0U);
    EXPECT_LE(tree.GetStatistics().MaxDepth, 16U);
    const auto capacity = tree.GetStatistics().AllocatedNodes;
    std::size_t rotations = 0;
    for (std::uint32_t run = 1; run <= 100; ++run) {
        for (std::uint32_t index = 0; index < 32; ++index) {
            const auto slot = (run * 37 + index * 13) % static_cast<std::uint32_t>(records.size());
            auto& record = records[slot];
            if (record.Validity == NScene::ESpatialValidity::VALID && index % 4 == 0) {
                record.Validity = NScene::ESpatialValidity::DISABLED;
            } else {
                if (record.Validity != NScene::ESpatialValidity::VALID) {
                    ++record.Identity.Entity.Identity.Generation;
                }
                record.Validity = NScene::ESpatialValidity::VALID;
                record.Bounds->Aabb = Box(static_cast<double>((slot * 19 + run * 7) % 512) - 256,
                                          static_cast<double>(index % 8),
                                          static_cast<double>(index % 5));
            }
        }
        auto input = records;
        std::reverse(input.begin(), input.end());
        tree = tree.Refresh(static_cast<std::uint64_t>(run) + 1, input);
        rotations += tree.GetStatistics().Rotations;
        EXPECT_LE(tree.GetStatistics().AllocatedNodes, capacity);
        EXPECT_LE(tree.GetStatistics().MaxDepth, 16U);
        const double x = static_cast<double>((run * 11) % 512) - 256;
        const SpatialAabb query{{x, -1, -1}, {x + 100, 8, 8}};
        const SpatialAabb rayBox{{x, 0, 0}, {x + 100, 0, 0}};
        std::vector<SpatialIdentity> expected;
        std::vector<SpatialIdentity> expectedRay;
        for (const auto& record: records) {
            if (record.Validity != NScene::ESpatialValidity::VALID) {
                continue;
            }
            if (Overlaps(record.Bounds->Aabb, query)) {
                expected.push_back(record.Identity);
            }
            if (Overlaps(record.Bounds->Aabb, rayBox)) {
                expectedRay.push_back(record.Identity);
            }
        }
        EXPECT_EQ(tree.QueryOverlap(query).Candidates, expected);
        EXPECT_EQ(tree.QueryFrustum(Frustum(query)).Candidates, expected);
        EXPECT_EQ(tree.QueryRay({.Origin = {x, 0, 0}, .MaxDistance = 100}).Candidates, expectedRay);
    }
    EXPECT_GT(rotations, 0U);
}

TEST_F(DynamicAabbTreeTest, ReadersRetainExactSnapshotAcrossConcurrentRefreshAndPublication) {
    auto records = Grid(1);
    records[0].Bounds->Aabb = Box(4);
    std::atomic<std::shared_ptr<const DynamicAabbTree>> published{
            std::make_shared<const DynamicAabbTree>(DynamicAabbTree::Build(Scene, 1, records))};
    std::barrier phases{5};
    std::atomic<bool> valid = true;
    const auto identity = records[0].Identity;
    std::vector<std::jthread> readers;
    for (int reader = 0; reader < 4; ++reader) {
        readers.emplace_back([&] {
            for (std::uint64_t version = 1; version <= 64; ++version) {
                const auto snapshot = published.load();
                phases.arrive_and_wait();
                const auto hits = snapshot->QueryOverlap(Box(static_cast<double>(version) * 4));
                if (hits.Version != version || hits.Candidates != std::vector{identity} ||
                    !snapshot->QueryOverlap(Box(static_cast<double>(version + 1) * 4)).Candidates.empty()) {
                    valid.store(false);
                }
                phases.arrive_and_wait();
            }
        });
    }
    for (std::uint64_t version = 1; version <= 64; ++version) {
        phases.arrive_and_wait();
        records[0].Bounds->Aabb = Box(static_cast<double>(version + 1) * 4);
        const auto next = published.load()->Refresh(version + 1, records);
        published.store(std::make_shared<const DynamicAabbTree>(next));
        phases.arrive_and_wait();
    }
    readers.clear();
    EXPECT_TRUE(valid.load());
    EXPECT_EQ(published.load()->GetVersion(), 65U);
}

TEST_F(DynamicAabbTreeTest, CommittedCaptureTracksHierarchyAndRecycledEntityWithoutPinningScene) {
    auto lease = Scenes.Acquire(Scene);
    NScene::SpatialBoundsComponent bounds;
    bounds.LocalBounds = Box(0);
    bounds.Mobility = NScene::ESpatialMobility::DYNAMIC;
    const auto parent = lease->CreateEntity();
    auto entity = lease->CreateEntity().AddComponent<NScene::SpatialBoundsComponent>(bounds);
    lease->CommitPending();
    auto& world = lease->GetWorld();
    world.QueueReparent(entity.GetReference(), parent.GetReference());
    lease->CommitPending();
    const auto first = DynamicAabbTree::Build(Scene, 1, NScene::CaptureSpatialRecords(Scenes, Scene));
    const SpatialIdentity oldIdentity{Scene, entity.GetReference()};
    ASSERT_EQ(first.QueryOverlap(Box(0)).Candidates, std::vector{oldIdentity});
    world.SetLocalTransform(parent.GetReference(), NMath::ComposeTransform({.Translation = {100, 0, 0}}));
    lease->CommitPending();
    const auto moved = first.Refresh(2, NScene::CaptureSpatialRecords(Scenes, Scene));
    EXPECT_EQ(moved.GetStatistics().Reinserted, 1U);
    EXPECT_EQ(moved.QueryOverlap(Box(100)).Candidates, std::vector{oldIdentity});
    entity.Destroy();
    const auto pending = moved.Refresh(3, NScene::CaptureSpatialRecords(Scenes, Scene));
    EXPECT_EQ(pending.QueryOverlap(Box(100)).Candidates, std::vector{oldIdentity});
    lease->CommitPending();
    auto replacement = lease->CreateEntity().AddComponent<NScene::SpatialBoundsComponent>(bounds);
    lease->CommitPending();
    const SpatialIdentity newIdentity{Scene, replacement.GetReference()};
    ASSERT_EQ(oldIdentity.Entity.Identity.Index, newIdentity.Entity.Identity.Index);
    ASSERT_NE(oldIdentity.Entity.Identity.Generation, newIdentity.Entity.Identity.Generation);
    const auto latest = pending.Refresh(4, NScene::CaptureSpatialRecords(Scenes, Scene));
    EXPECT_EQ(latest.QueryOverlap(Box(0)).Candidates, std::vector{newIdentity});
    EXPECT_TRUE(latest.QueryOverlap(Box(100)).Candidates.empty());
    EXPECT_FALSE(NScene::IsSpatialCandidateAlive(Scenes, oldIdentity));
    EXPECT_TRUE(NScene::IsSpatialCandidateAlive(Scenes, newIdentity));
    const std::weak_ptr<NScene::Scene> weak = lease;
    Scenes.Unload(Scene);
    lease.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(NScene::IsSpatialCandidateAlive(Scenes, newIdentity));
    EXPECT_EQ(latest.QueryOverlap(Box(0)).Candidates, std::vector{newIdentity});
    EXPECT_EQ(first.QueryOverlap(Box(0)).Candidates, std::vector{oldIdentity});
}

} // namespace
