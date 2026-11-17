#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <ecs/internal/adaptive_traversal.h>
#include <gtest/gtest.h>
#include <tests/ecs_concurrency/forest.h>

namespace {
TEST(EcsAcceptanceScale, LargeShapesKeepPacketBudgetAndIndependentSerialResults) {
    for (const auto shape: NEcsAcceptance::SHAPES) {
        for (const unsigned workers: {1U, 16U}) {
            SCOPED_TRACE(testing::Message() << "shape=" << static_cast<int>(shape) << " workers=" << workers);
            NEcs::World world;
            NEcsAcceptance::Forest forest{world, 65536, shape};
            const auto reference = forest.TraverseReference(false, false, 32);
            NCommon::TaskSystem tasks{workers};
            NEcs::NInternal::AdaptiveBatchPlanner planner{tasks};
            const std::array roots{forest.Entities.front()};
            NEcs::NInternal::TraversalOperation<std::uint64_t> operation;
            operation.Output = std::make_shared<NEcs::NInternal::TraversalOutput<std::uint64_t>>();
            operation.Visit = [](auto input) {
                return NEcsAcceptance::Work((input.ParentResult ? *input.ParentResult : 17) * 31 +
                                                    input.Entity.Identity.Index,
                                            32);
            };
            const auto output = operation.Output;
            const auto group = planner.ScheduleTraversal(world.AcquireTopologySnapshot(),
                                                         roots,
                                                         NEcs::NInternal::ETraversalMode::TOP_DOWN,
                                                         std::move(operation));
            tasks.WaitGroup(group);
            const auto actual = output->TryAcquire(tasks);
            ASSERT_TRUE(actual);
            for (std::size_t index = 0; index < forest.Entities.size(); ++index) {
                ASSERT_EQ(actual->Get(forest.Entities[index]), reference[index]) << index;
            }
            EXPECT_LE(output->GetMetrics().PeakActivePackets, workers * 2U);
            if (shape == NEcsAcceptance::EShape::BAMBOO || workers == 1) {
                EXPECT_EQ(output->GetMetrics().Packets, 1U);
            }
        }
    }
}

TEST(EcsAcceptanceScale, MillionNodeStarDoesNotCreateOneTaskPerEntity) {
    NEcs::World world;
    constexpr std::size_t count = 1000000;
    NEcsAcceptance::Forest forest{world, count, NEcsAcceptance::EShape::STAR};
    NCommon::TaskSystem tasks{16};
    NEcs::NInternal::AdaptiveBatchPlanner planner{tasks};
    NEcs::NInternal::TraversalOperation<std::uint64_t> operation;
    operation.Output = std::make_shared<NEcs::NInternal::TraversalOutput<std::uint64_t>>();
    operation.Visit = [](auto input) { return static_cast<std::uint64_t>(input.Entity.Identity.Index); };
    const auto output = operation.Output;
    const std::array roots{forest.Entities.front()};
    const auto group = planner.ScheduleTraversal(world.AcquireTopologySnapshot(),
                                                 roots,
                                                 NEcs::NInternal::ETraversalMode::INDEPENDENT,
                                                 std::move(operation));
    tasks.WaitGroup(group);
    const auto actual = output->TryAcquire(tasks);
    ASSERT_TRUE(actual);
    for (const auto entity: forest.Entities) {
        ASSERT_EQ(actual->Get(entity), entity.Identity.Index);
    }
    const auto metrics = output->GetMetrics();
    EXPECT_EQ(metrics.VisitedNodes, count);
    EXPECT_LE(metrics.PeakActivePackets, 32U);
    EXPECT_LT(metrics.Packets, count / 100);
    RecordProperty("packets", static_cast<int>(metrics.Packets));
    RecordProperty("peak_packets", static_cast<int>(metrics.PeakActivePackets));
}
} // namespace
