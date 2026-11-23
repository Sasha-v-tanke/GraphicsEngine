#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <GraphicsEngine/scene/scene_manager.h>
#include <GraphicsEngine/scene/static_bvh.h>
#include <gtest/gtest.h>
#include <scene/test/bvh_fixture.h>
#include <tests/common/test_error.h>

namespace {

using NScene::SpatialAabb;
using NScene::SpatialIdentity;
using NScene::SpatialRecord;
using NScene::StaticBvh;

bool Overlap(SpatialAabb a, SpatialAabb b) {
    return a.Max.X >= b.Min.X && b.Max.X >= a.Min.X && a.Max.Y >= b.Min.Y && b.Max.Y >= a.Min.Y && a.Max.Z >= b.Min.Z &&
           b.Max.Z >= a.Min.Z;
}

bool RayReference(SpatialAabb box, NScene::SpatialRay ray) {
    const std::array<long double, 3> min{box.Min.X, box.Min.Y, box.Min.Z};
    const std::array<long double, 3> max{box.Max.X, box.Max.Y, box.Max.Z};
    const std::array<long double, 3> origin{ray.Origin.X, ray.Origin.Y, ray.Origin.Z};
    const std::array<long double, 3> direction{ray.Direction.X, ray.Direction.Y, ray.Direction.Z};
    long double enter = ray.MinDistance;
    long double exit = ray.MaxDistance;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0) {
            if (origin[axis] < min[axis] || origin[axis] > max[axis]) {
                return false;
            }
            continue;
        }
        const long double one = (min[axis] - origin[axis]) / direction[axis];
        const long double two = (max[axis] - origin[axis]) / direction[axis];
        enter = std::max(enter, std::min(one, two));
        exit = std::min(exit, std::max(one, two));
    }
    return enter <= exit;
}

bool FrustumReference(SpatialAabb box, const NScene::SpatialFrustum& frustum) {
    for (const auto& plane: frustum.Planes) {
        long double maximum = -std::numeric_limits<long double>::infinity();
        for (int mask = 0; mask < 8; ++mask) {
            const long double x = (mask & 1) != 0 ? box.Max.X : box.Min.X;
            const long double y = (mask & 2) != 0 ? box.Max.Y : box.Min.Y;
            const long double z = (mask & 4) != 0 ? box.Max.Z : box.Min.Z;
            maximum = std::max(maximum, plane.Normal.X * x + plane.Normal.Y * y + plane.Normal.Z * z + plane.Offset);
        }
        if (maximum < 0) {
            return false;
        }
    }
    return true;
}

NScene::SpatialFrustum Frustum(double half) {
    return {{{{{1, 0, 0}, half},
              {{-1, 0, 0}, half},
              {{0, 1, 0}, half},
              {{0, -1, 0}, half},
              {{0, 0, 1}, half},
              {{0, 0, -1}, half}}}};
}

class StaticBvhTest: public testing::Test {
protected:
    NScene::SceneManager Scenes;
    NScene::SceneHandle Scene = Scenes.CreateScene();

    std::vector<SpatialRecord> Grid(std::uint32_t count, double offset = 0) {
        std::vector<SpatialRecord> records;
        records.reserve(count);
        for (std::uint32_t item = 0; item < count; ++item) {
            const double x = static_cast<double>(item) * 4 + offset;
            records.push_back(NBvhTest::Record(Scene, item, {{x, 0, 0}, {x + 1, 1, 1}}));
        }
        return records;
    }
};

TEST_F(StaticBvhTest, EmptyAndSkippedRecordsHaveVersionedDeterministicCounters) {
    auto input = Grid(3);
    input[0].Validity = NScene::ESpatialValidity::DISABLED;
    input[1].Mobility = NScene::ESpatialMobility::DYNAMIC;
    input[2].Mobility.reset();
    const auto index = StaticBvh::Build(Scene, 7, input);
    EXPECT_TRUE(index.GetNodes().empty());
    EXPECT_EQ(index.GetScene(), Scene);
    EXPECT_EQ(index.GetVersion(), 7U);
    EXPECT_EQ(index.GetStatistics().SkippedRecords, 3U);
    EXPECT_EQ(index.GetStatistics().IndexedRecords, 0U);
    EXPECT_EQ(index.GetStatistics().Nodes, 0U);
    EXPECT_NE(index.GetStatistics().Serialize().find("\"version\":7"), std::string::npos);
    const auto result = index.QueryRay({});
    EXPECT_EQ(result.Version, 7U);
    EXPECT_TRUE(result.Candidates.empty());
    EXPECT_EQ(result.Statistics.Serialize(), R"({"schema":1,"visited_nodes":0,"tested_items":0,"matched_items":0})");
    EXPECT_TRUE(index.QueryOverlap({}).Candidates.empty());
    EXPECT_TRUE(index.QueryFrustum(Frustum(1)).Candidates.empty());
    EXPECT_EQ(StaticBvh::Build(Scene, 1, {}).GetStatistics().InputRecords, 0U);
}

TEST_F(StaticBvhTest, RejectsMalformedIncludedRecordsBudgetsAndVersions) {
    auto input = Grid(2);
    const auto reject = [&](const std::vector<SpatialRecord>& records) {
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)StaticBvh::Build(Scene, 1, records); });
    };
    auto bad = input;
    bad[1].Identity = bad[0].Identity;
    reject(bad);
    bad = input;
    bad[0].Identity.Scene = Scenes.CreateScene();
    reject(bad);
    bad = input;
    bad[0].Identity.Entity.WorldId = 0;
    reject(bad);
    bad = input;
    ++bad[0].Identity.Entity.WorldId;
    reject(bad);
    bad = input;
    bad[0].SourceRevision = 0;
    reject(bad);
    bad = input;
    bad[0].Bounds.reset();
    reject(bad);
    for (double value: {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1e100}) {
        bad = input;
        bad[0].Bounds->Aabb.Min.X = value;
        reject(bad);
    }
    bad = input;
    bad[0].Bounds->Aabb.Max.X = -1;
    reject(bad);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)StaticBvh::Build({}, 1, {}); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)StaticBvh::Build(Scene, 0, {}); });
    for (NScene::StaticBvhConfig config: {NScene::StaticBvhConfig{0, 8}, {1, 8}, {100, 0}, {100, 65}}) {
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)StaticBvh::Build(Scene, 1, input, config); });
    }
}

TEST_F(StaticBvhTest, DegenerateOverlapsAndTieOrderingDoNotDependOnInputOrder) {
    auto input = Grid(129);
    for (auto& record: input) {
        record.Bounds->Aabb = {};
    }
    const auto first = StaticBvh::Build(Scene, 1, input, {.LeafCapacity = 1});
    std::reverse(input.begin(), input.end());
    const auto second = StaticBvh::Build(Scene, 1, input, {.LeafCapacity = 1});
    EXPECT_TRUE(std::ranges::equal(first.GetNodes(), second.GetNodes()));
    EXPECT_EQ(first.GetStatistics().Serialize(), second.GetStatistics().Serialize());
    const auto hits = first.QueryOverlap({});
    ASSERT_EQ(hits.Candidates.size(), input.size());
    for (std::size_t item = 0; item < hits.Candidates.size(); ++item) {
        EXPECT_EQ(hits.Candidates[item].Entity.Identity.Index, item);
    }
    EXPECT_EQ(first.QueryRay({.Origin = {-1, 0, 0}}).Candidates, hits.Candidates);
    EXPECT_EQ(first.QueryFrustum(Frustum(0)).Candidates, hits.Candidates);
}

TEST_F(StaticBvhTest, RandomizedRayFrustumAndOverlapMatchIndependentBruteForce) {
    // Reproducible non-security regression corpus.
    // NOLINTNEXTLINE(cert-msc51-cpp)
    std::mt19937 random{116};
    std::uniform_real_distribution<double> position{-100, 100};
    std::uniform_real_distribution<double> size{0, 5};
    std::vector<SpatialRecord> records;
    for (std::uint32_t item = 0; item < 2048; ++item) {
        const double x = position(random);
        const double y = position(random);
        const double z = position(random);
        records.push_back(
                NBvhTest::Record(Scene, item, {{x, y, z}, {x + size(random), y + size(random), z + size(random)}}));
    }
    const auto index = StaticBvh::Build(Scene, 1, records);
    for (int run = 0; run < 64; ++run) {
        const double x = position(random);
        const double y = position(random);
        const double z = position(random);
        const SpatialAabb overlap{{x, y, z}, {x + 30, y + 30, z + 30}};
        const NScene::SpatialRay ray{{x, y, z}, {position(random), position(random), position(random)}, 0, 5};
        auto frustum = Frustum(30);
        const double angle = static_cast<double>(run) * 0.13;
        for (auto& plane: frustum.Planes) {
            const auto old = plane.Normal;
            plane.Normal = {std::cos(angle) * old.X - std::sin(angle) * old.Y,
                            std::sin(angle) * old.X + std::cos(angle) * old.Y,
                            old.Z};
        }
        std::vector<SpatialIdentity> boxes;
        std::vector<SpatialIdentity> rays;
        std::vector<SpatialIdentity> frusta;
        for (const auto& record: records) {
            if (Overlap(record.Bounds->Aabb, overlap)) {
                boxes.push_back(record.Identity);
            }
            if (RayReference(record.Bounds->Aabb, ray)) {
                rays.push_back(record.Identity);
            }
            if (FrustumReference(record.Bounds->Aabb, frustum)) {
                frusta.push_back(record.Identity);
            }
        }
        EXPECT_EQ(index.QueryOverlap(overlap).Candidates, boxes);
        EXPECT_EQ(index.QueryRay(ray).Candidates, rays);
        EXPECT_EQ(index.QueryFrustum(frustum).Candidates, frusta);
    }
}

TEST_F(StaticBvhTest, CameraRelativeQueriesPreserveGlobalIndexAndParallelRayBoundaries) {
    auto records = Grid(2, 1e9);
    const auto index = StaticBvh::Build(Scene, 1, records);
    const auto before = index.GetStatistics().Serialize();
    const NScene::SpatialVector origin{1e9, 0, 0};
    const auto overlap = index.QueryOverlap({{0, 0, 0}, {1, 1, 1}}, origin);
    ASSERT_EQ(overlap.Candidates.size(), 1U);
    EXPECT_EQ(overlap.Candidates[0], records[0].Identity);
    EXPECT_EQ(index.QueryRay({.Origin = {-1, 0, 0}, .MaxDistance = 2}, origin).Candidates, overlap.Candidates);
    EXPECT_TRUE(index.QueryRay({.Origin = {-1, 2, 0}}, origin).Candidates.empty());
    EXPECT_EQ(index.QueryRay({.Origin = {2, 0, 0}, .Direction = {-1, 0, 0}, .MaxDistance = 1}, origin).Candidates,
              overlap.Candidates);
    EXPECT_EQ(index.QueryFrustum(Frustum(1), origin).Candidates, overlap.Candidates);
    EXPECT_EQ(index.GetStatistics().Serialize(), before);
    const auto point = StaticBvh::Build(Scene, 1, std::span{records}.first(1));
    EXPECT_EQ(point.QueryRay({.Origin = {-1, 0, 0}, .Direction = {1e-300, 0, 0}}, origin).Candidates.size(), 1U);
}

TEST_F(StaticBvhTest, InvalidQueriesFailEvenForEmptyIndex) {
    const auto index = StaticBvh::Build(Scene, 1, {});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)index.QueryOverlap({{2, 0, 0}, {1, 1, 1}}); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)index.QueryOverlap({}, {nan, 0, 0}); });
    for (NScene::SpatialRay ray: {NScene::SpatialRay{.Direction = {}},
                                  {.Origin = {nan, 0, 0}},
                                  {.MinDistance = -1},
                                  {.MinDistance = 2, .MaxDistance = 1},
                                  {.MaxDistance = nan}}) {
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)index.QueryRay(ray); });
    }
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)index.QueryFrustum({}); });
    auto frustum = Frustum(1);
    frustum.Planes[0].Offset = nan;
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)index.QueryFrustum(frustum); });
}

TEST_F(StaticBvhTest, RefitPreservesReadersAndRequiresSameMembershipAndNewVersion) {
    auto records = Grid(128);
    const auto old = StaticBvh::Build(Scene, 1, records);
    const auto oldNodes = std::vector(old.GetNodes().begin(), old.GetNodes().end());
    auto moved = Grid(128, 1000);
    std::reverse(moved.begin(), moved.end());
    const auto next = old.Refit(2, moved);
    const SpatialAabb query{{0, 0, 0}, {1, 1, 1}};
    EXPECT_EQ(old.QueryOverlap(query).Candidates.size(), 1U);
    EXPECT_TRUE(next.QueryOverlap(query).Candidates.empty());
    EXPECT_TRUE(std::ranges::equal(old.GetNodes(), oldNodes));
    ASSERT_EQ(next.GetNodes().size(), old.GetNodes().size());
    for (std::size_t node = 0; node < oldNodes.size(); ++node) {
        EXPECT_EQ(next.GetNodes()[node].Left, oldNodes[node].Left);
        EXPECT_EQ(next.GetNodes()[node].Right, oldNodes[node].Right);
        EXPECT_EQ(next.GetNodes()[node].Count, oldNodes[node].Count);
        EXPECT_EQ(next.GetNodes()[node].First, oldNodes[node].First);
    }
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)next.Refit(2, moved); });
    moved.pop_back();
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)next.Refit(3, moved); });
    moved = records;
    ++moved[0].Identity.Entity.Identity.Generation;
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)old.Refit(2, moved); });
    EXPECT_EQ(StaticBvh::Build(Scene, 3, moved).GetStatistics().IndexedRecords, records.size());
}

TEST_F(StaticBvhTest, RebuildAndRefitPublishWhileConcurrentReadersKeepOldSnapshots) {
    const auto first = std::make_shared<const StaticBvh>(StaticBvh::Build(Scene, 1, Grid(4096)));
    std::shared_ptr<const StaticBvh> published = first;
    std::mutex publication;
    const auto acquire = [&] {
        const std::lock_guard lock{publication};
        return published;
    };
    std::latch entered{4};
    std::atomic_bool done = false;
    std::atomic_bool failed = false;
    std::vector<std::jthread> readers;
    readers.reserve(4);
    for (int thread = 0; thread < 4; ++thread) {
        readers.emplace_back([&] {
            entered.count_down();
            do {
                const auto snapshot = acquire();
                const auto hits = snapshot->QueryOverlap({{-1, -1, -1}, {20'000, 2, 2}});
                if (hits.Version != snapshot->GetVersion() ||
                    hits.Candidates.size() != (hits.Version == 1 ? 4096U : 0U)) {
                    failed = true;
                }
            } while (!done);
        });
    }
    entered.wait();
    for (std::uint64_t version = 2; version <= 8; ++version) {
        const auto current = acquire();
        const auto records = Grid(4096, static_cast<double>(version) * 100'000);
        const auto next = std::make_shared<const StaticBvh>(version % 2 == 0 ? StaticBvh::Build(Scene, version, records)
                                                                             : current->Refit(version, records));
        const std::lock_guard lock{publication};
        published = next;
    }
    done = true;
    readers.clear();
    EXPECT_FALSE(failed);
    EXPECT_EQ(first->GetVersion(), 1U);
    EXPECT_EQ(first->QueryOverlap({{-1, -1, -1}, {20'000, 2, 2}}).Candidates.size(), 4096U);
}

TEST_F(StaticBvhTest, OldCandidatesRejectDisabledRecycledAndUnloadedSceneIdentities) {
    auto scene = Scenes.Acquire(Scene);
    NScene::SpatialBoundsComponent bounds;
    bounds.Mobility = NScene::ESpatialMobility::STATIC;
    auto entity = scene->CreateEntity().AddComponent<NScene::SpatialBoundsComponent>(bounds);
    scene->CommitPending();
    const auto input = NScene::CaptureSpatialRecords(Scenes, Scene);
    const auto index = StaticBvh::Build(Scene, 1, input);
    const auto hits = index.QueryOverlap({{-1, -1, -1}, {1, 1, 1}});
    ASSERT_EQ(hits.Candidates.size(), 1U);
    const auto identity = hits.Candidates[0];
    EXPECT_TRUE(NScene::IsSpatialCandidateAlive(Scenes, identity));
    scene->GetWorld().QueueSetEnabled(entity.GetReference(), false);
    scene->CommitPending();
    EXPECT_FALSE(NScene::IsSpatialCandidateAlive(Scenes, identity));
    entity.Destroy();
    scene->CommitPending();
    auto replacement = scene->CreateEntity();
    scene->CommitPending();
    EXPECT_EQ(replacement.GetReference().Identity.Index, identity.Entity.Identity.Index);
    EXPECT_NE(replacement.GetReference(), identity.Entity);
    EXPECT_FALSE(NScene::IsSpatialCandidateAlive(Scenes, identity));
    EXPECT_EQ(index.QueryOverlap({{-1, -1, -1}, {1, 1, 1}}).Candidates, hits.Candidates);
    const std::weak_ptr<NScene::Scene> weak = scene;
    Scenes.Unload(Scene);
    scene.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(NScene::IsSpatialCandidateAlive(Scenes, identity));
    EXPECT_EQ(index.QueryOverlap({{-1, -1, -1}, {1, 1, 1}}).Candidates, hits.Candidates);
}

} // namespace
