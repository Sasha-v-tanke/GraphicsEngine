#include <chrono>
#include <iostream>
#include <vector>

#include <GraphicsEngine/scene/scene_manager.h>
#include <GraphicsEngine/scene/static_bvh.h>
#include <gtest/gtest.h>
#include <scene/test/bvh_fixture.h>

namespace {
TEST(StaticBvhScale, MillionBoundsHaveBoundedMemoryDepthAndLocalizedQueryCost) {
    NScene::SceneManager scenes;
    const auto scene = scenes.CreateScene();
    std::vector<NScene::SpatialRecord> records;
    records.reserve(1'000'000);
    for (std::uint32_t item = 0; item < 1'000'000; ++item) {
        const double x = static_cast<double>(item % 100) * 4;
        const double y = static_cast<double>((item / 100) % 100) * 4;
        const auto layer = item / 10'000;
        const double z = static_cast<double>(layer) * 4;
        records.push_back(NBvhTest::Record(scene, item, {{x, y, z}, {x + 1, y + 1, z + 1}}));
    }
    const auto start = std::chrono::steady_clock::now();
    const auto index = NScene::StaticBvh::Build(scene, 1, records);
    const auto built = std::chrono::steady_clock::now();
    const auto& statistics = index.GetStatistics();
    EXPECT_EQ(statistics.IndexedRecords, records.size());
    EXPECT_LE(statistics.StorageBytes, 160U * 1024U * 1024U);
    EXPECT_LE(statistics.MaxDepth, 22U);
    std::size_t visited = 0;
    for (std::uint32_t query = 0; query < 1000; ++query) {
        const auto item = query * 997;
        const auto bounds = records[item].Bounds->Aabb;
        const auto hits = index.QueryOverlap(bounds);
        ASSERT_EQ(hits.Candidates.size(), 1U);
        EXPECT_EQ(hits.Candidates[0], records[item].Identity);
        EXPECT_LT(hits.Statistics.TestedItems, 1000U);
        visited += hits.Statistics.VisitedNodes;
    }
    const auto queried = std::chrono::steady_clock::now();
    std::cout << "StaticBvh scale: " << statistics.Serialize()
              << " build_ms=" << std::chrono::duration<double, std::milli>(built - start).count()
              << " query_us=" << std::chrono::duration<double, std::micro>(queried - built).count() / 1000.0
              << " average_visited_nodes=" << static_cast<double>(visited) / 1000.0 << '\n';
}
} // namespace
