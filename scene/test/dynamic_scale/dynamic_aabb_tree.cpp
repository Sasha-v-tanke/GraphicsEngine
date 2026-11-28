#include <chrono>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

#include <GraphicsEngine/scene/dynamic_aabb_tree.h>
#include <GraphicsEngine/scene/scene_manager.h>
#include <gtest/gtest.h>
#include <scene/test/bvh_fixture.h>

namespace {

TEST(DynamicAabbTreeScale, CompareIncrementalRefreshWithFullRebuildAtLowChurn) {
    NScene::SceneManager scenes;
    const auto scene = scenes.CreateScene();
    constexpr std::uint32_t count = 50'000;
    constexpr std::uint64_t rounds = 8;
    std::vector<NScene::SpatialRecord> records;
    records.reserve(count);
    for (std::uint32_t item = 0; item < count; ++item) {
        const double x = static_cast<double>(item % 100) * 4;
        const double y = static_cast<double>((item / 100) % 100) * 4;
        const double z = static_cast<double>(item / 10'000) * 4;
        auto record = NBvhTest::Record(scene, item, {{x, y, z}, {x + 1, y + 1, z + 1}});
        record.Mobility = NScene::ESpatialMobility::DYNAMIC;
        records.push_back(std::move(record));
    }

    auto index = NScene::DynamicAabbTree::Build(scene, 1, records);
    const auto capacity = index.GetStatistics().AllocatedNodes;
    double incrementalMs = 0;
    double dynamicRebuildMs = 0;
    double staticRebuildMs = 0;
    std::size_t reinserts = 0;
    std::size_t rotations = 0;
    for (std::uint64_t round = 0; round < rounds; ++round) {
        // One percent of leaves move; every tenth changed leaf teleports.
        for (std::uint32_t item = static_cast<std::uint32_t>(round); item < count; item += 100) {
            const double shift = item % 1000 < 100 ? 1000.0 : 0.125;
            auto& record = records[item];
            record.Bounds->Aabb.Min.X += shift;
            record.Bounds->Aabb.Max.X += shift;
            ++record.SourceRevision;
        }
        auto staticRecords = records;
        for (auto& record: staticRecords) {
            record.Mobility = NScene::ESpatialMobility::STATIC;
        }

        const auto start = std::chrono::steady_clock::now();
        index = index.Refresh(round + 2, records);
        const auto refreshed = std::chrono::steady_clock::now();
        const auto rebuilt = NScene::DynamicAabbTree::Build(scene, round + 2, records);
        const auto dynamicFinished = std::chrono::steady_clock::now();
        const auto staticIndex = NScene::StaticBvh::Build(scene, round + 2, staticRecords);
        const auto staticFinished = std::chrono::steady_clock::now();
        incrementalMs += std::chrono::duration<double, std::milli>(refreshed - start).count();
        dynamicRebuildMs += std::chrono::duration<double, std::milli>(dynamicFinished - refreshed).count();
        staticRebuildMs += std::chrono::duration<double, std::milli>(staticFinished - dynamicFinished).count();
        const auto& stats = index.GetStatistics();
        EXPECT_EQ(stats.IndexedRecords, count);
        EXPECT_EQ(stats.Inserted, 0U);
        EXPECT_EQ(stats.Removed, 0U);
        EXPECT_EQ(stats.Reinserted, count / 1000);
        EXPECT_EQ(stats.UpdatedInFatBounds, count / 100 - count / 1000);
        EXPECT_LE(stats.MaxDepth, 32U);
        EXPECT_EQ(stats.AllocatedNodes, capacity);
        reinserts += stats.Reinserted;
        rotations += stats.Rotations;
        for (std::uint32_t query = 0; query < 100; ++query) {
            const auto& box = records[query * 499].Bounds->Aabb;
            const auto hits = index.QueryOverlap(box);
            EXPECT_EQ(hits.Candidates, rebuilt.QueryOverlap(box).Candidates);
            EXPECT_EQ(hits.Candidates, staticIndex.QueryOverlap(box).Candidates);
            EXPECT_FALSE(hits.Candidates.empty());
        }
    }
    std::cout << "DynamicAabbTree scale: records=" << count << " rounds=" << rounds << " refresh_ms=" << incrementalMs
              << " dynamic_rebuild_ms=" << dynamicRebuildMs << " static_rebuild_ms=" << staticRebuildMs
              << " reinserts=" << reinserts << " rotations=" << rotations << '\n';
}

} // namespace
