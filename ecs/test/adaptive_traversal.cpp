#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <random>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

#include <GraphicsEngine/ecs/world.h>
#include <ecs/internal/adaptive_traversal.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {
using NEcs::EntityReference;
using NEcs::World;
using NEcs::NInternal::AdaptiveBatchPlanner;
using NEcs::NInternal::BatchPolicy;
using NEcs::NInternal::ETraversalMode;
using NEcs::NInternal::TraversalInput;
using NEcs::NInternal::TraversalOperation;
using NEcs::NInternal::TraversalOutput;
using namespace std::chrono_literals;

enum class EShape {
    BALANCED,
    STAR,
    BAMBOO,
    RANDOM,
    SKEWED
};
struct Forest {
    World Owner;
    std::vector<EntityReference> Entities;
    std::vector<EntityReference> Roots;
    explicit Forest(std::size_t count, EShape shape) {
        // NOLINTNEXTLINE(cert-msc51-cpp): Fixed seed makes the topology reproducible.
        std::mt19937 random{73};
        std::vector<std::size_t> parents(count);
        for (std::size_t index = 0; index < count; ++index) {
            Entities.push_back(Owner.GetEntityReference(Owner.CreateEntity()));
            if (index == 0) {
                Roots.push_back(Entities.back());
                continue;
            }
            std::size_t parent = 0;
            switch (shape) {
            case EShape::BALANCED:
                parent = (index - 1) / 3;
                break;
            case EShape::STAR:
                break;
            case EShape::BAMBOO:
                parent = index - 1;
                break;
            case EShape::RANDOM:
                parent = random() % index;
                break;
            case EShape::SKEWED:
                parent = index % 5 == 0 ? 0 : index - 1;
                break;
            }
            parents[index] = parent;
        }
        // Attach descendants first so deep-chain fixture construction is linear.
        for (std::size_t index = count; index > 1; --index) {
            Owner.DeferReparent(Entities[index - 1], Entities[parents[index - 1]]);
        }
        Owner.ApplyDeferredStructuralChanges();
    }
};
TraversalOperation<std::uint64_t> Operation() {
    return {.Visit =
                    [](TraversalInput<std::uint64_t> input) {
                        std::uint64_t value = input.ParentResult ? *input.ParentResult : 17;
                        value = value * 31 + input.Entity.Identity.Index;
                        for (const auto* child: input.ChildResults) {
                            value = value * 31 + *child;
                        }
                        return value;
                    },
            .Output = std::make_shared<TraversalOutput<std::uint64_t>>()};
}

TEST(AdaptiveTraversal, SerialEquivalenceExactOnceAndDeterministicReductionAcrossShapes) {
    for (const auto shape: {EShape::BALANCED, EShape::STAR, EShape::BAMBOO, EShape::RANDOM, EShape::SKEWED}) {
        Forest forest{300, shape};
        for (const auto mode: {ETraversalMode::TOP_DOWN, ETraversalMode::BOTTOM_UP, ETraversalMode::INDEPENDENT}) {
            const auto serial = AdaptiveBatchPlanner::RunSerial(forest.Owner.AcquireTopologySnapshot(),
                                                                forest.Roots,
                                                                mode,
                                                                Operation());
            for (const unsigned workers: {1U, 2U, 4U}) {
                NCommon::TaskSystem tasks{workers};
                AdaptiveBatchPlanner planner{tasks};
                auto operation = Operation();
                std::array<std::atomic_size_t, 300> visits{};
                const auto visitor = operation.Visit;
                operation.Visit = [&, visitor](TraversalInput<std::uint64_t> input) {
                    visits[input.Entity.Identity.Index].fetch_add(1);
                    if (input.Entity.Identity.Index % 7 == 0) {
                        std::this_thread::yield();
                    }
                    return visitor(input);
                };
                operation.EstimateWork = [](EntityReference entity) {
                    return entity.Identity.Index % 7 == 0 ? 50.0 : 1.0;
                };
                const auto output = operation.Output;
                const auto group = planner.ScheduleTraversal(forest.Owner.AcquireTopologySnapshot(),
                                                             forest.Roots,
                                                             mode,
                                                             std::move(operation));
                tasks.WaitGroup(group);
                ASSERT_EQ(tasks.GetGroupStatus(group), NCommon::ETaskGroupStatus::COMPLETED);
                const auto actual = output->TryAcquire(tasks);
                ASSERT_TRUE(actual);
                for (const auto entity: forest.Entities) {
                    EXPECT_EQ(actual->Get(entity), serial.Get(entity));
                    EXPECT_EQ(visits[entity.Identity.Index].load(), 1U);
                }
                const auto metrics = output->GetMetrics();
                EXPECT_EQ(metrics.VisitedNodes, 300U);
                EXPECT_GE(metrics.Packets, 1U);
                EXPECT_LE(metrics.PeakActivePackets, workers * 2U);
                if (shape == EShape::BAMBOO || workers == 1) {
                    EXPECT_EQ(metrics.Packets, 1U);
                }
            }
        }
    }
}

TEST(AdaptiveTraversal, DeepBambooNeverRecursesOrSpawnsPerNode) {
    Forest forest{20000, EShape::BAMBOO};
    NCommon::TaskSystem tasks{4};
    AdaptiveBatchPlanner planner{tasks};
    for (const auto mode: {ETraversalMode::TOP_DOWN, ETraversalMode::BOTTOM_UP}) {
        auto operation = Operation();
        const auto output = operation.Output;
        const auto group = planner.ScheduleTraversal(forest.Owner.AcquireTopologySnapshot(),
                                                     forest.Roots,
                                                     mode,
                                                     std::move(operation));
        tasks.WaitGroup(group);
        EXPECT_EQ(tasks.GetGroupStatus(group), NCommon::ETaskGroupStatus::COMPLETED);
        EXPECT_EQ(output->GetMetrics().Packets, 1U);
        EXPECT_EQ(output->GetMetrics().VisitedNodes, 20000U);
    }
}

TEST(AdaptiveTraversal, HeavyIndependentBranchesActuallyOverlapAndBudgetIsBounded) {
    Forest forest{65, EShape::STAR};
    NCommon::TaskSystem tasks{4};
    AdaptiveBatchPlanner planner{tasks};
    std::atomic_size_t active{0};
    std::atomic_size_t peak{0};
    auto operation = Operation();
    const auto visitor = operation.Visit;
    operation.Visit = [&, visitor](TraversalInput<std::uint64_t> input) {
        const auto count = active.fetch_add(1) + 1;
        auto previous = peak.load();
        while (count > previous && !peak.compare_exchange_weak(previous, count)) {
        }
        std::this_thread::sleep_for(1ms);
        const auto result = visitor(input);
        active.fetch_sub(1);
        return result;
    };
    const auto output = operation.Output;
    const auto group = planner.ScheduleTraversal(forest.Owner.AcquireTopologySnapshot(),
                                                 forest.Roots,
                                                 ETraversalMode::TOP_DOWN,
                                                 std::move(operation));
    tasks.WaitGroup(group);
    EXPECT_EQ(tasks.GetGroupStatus(group), NCommon::ETaskGroupStatus::COMPLETED);
    EXPECT_GT(peak.load(), 1U);
    EXPECT_LE(output->GetMetrics().PeakActivePackets, 8U);
    EXPECT_LT(output->GetMetrics().Packets, forest.Entities.size());
}

TEST(AdaptiveTraversal, TopologyLeaseSurvivesPlannerAndWorldButRetiresBeforeGroupTerminal) {
    NCommon::TaskSystem tasks{1};
    auto world = std::make_shared<World>();
    const auto root = world->GetEntityReference(world->CreateEntity());
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    std::optional<NCommon::TaskGroupHandle> group;
    auto operation = Operation();
    const auto output = operation.Output;
    operation.Visit = [&](TraversalInput<std::uint64_t>) {
        entered.release();
        release.acquire();
        return std::uint64_t{42};
    };
    {
        AdaptiveBatchPlanner planner{tasks};
        group = planner.ScheduleTraversal(world->AcquireTopologySnapshot(),
                                          std::array{root},
                                          ETraversalMode::INDEPENDENT,
                                          std::move(operation));
    }
    EXPECT_TRUE(entered.try_acquire_for(5s));
    EXPECT_FALSE(output->TryAcquire(tasks));
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world->CreateEntity(); });
    world.reset();
    release.release();
    tasks.WaitGroup(*group);
    const auto result = output->TryAcquire(tasks);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->Get(root), 42U);

    World retained;
    const auto ref = retained.GetEntityReference(retained.CreateEntity());
    AdaptiveBatchPlanner planner{tasks};
    const auto next = planner.ScheduleTraversal(retained.AcquireTopologySnapshot(),
                                                std::array{ref},
                                                ETraversalMode::TOP_DOWN,
                                                Operation());
    tasks.WaitGroup(next);
    EXPECT_NO_THROW(retained.DestroyEntity(ref.Identity));
}

TEST(AdaptiveTraversal, FailureAndCancellationNeverExposePartialResults) {
    for (const bool failure: {false, true}) {
        Forest forest{100, EShape::BALANCED};
        NCommon::TaskSystem tasks{2};
        AdaptiveBatchPlanner planner{tasks};
        std::binary_semaphore entered{0};
        std::binary_semaphore release{0};
        auto operation = Operation();
        const auto output = operation.Output;
        operation.Visit = [&](TraversalInput<std::uint64_t>) -> std::uint64_t {
            entered.release();
            release.acquire();
            if (failure) {
                throw std::runtime_error{"node failed"};
            }
            return 1;
        };
        const auto group = planner.ScheduleTraversal(forest.Owner.AcquireTopologySnapshot(),
                                                     forest.Roots,
                                                     ETraversalMode::TOP_DOWN,
                                                     std::move(operation));
        EXPECT_TRUE(entered.try_acquire_for(5s));
        EXPECT_FALSE(output->TryAcquire(tasks));
        if (!failure) {
            tasks.CancelGroup(group);
        }
        release.release();
        tasks.WaitGroup(group);
        EXPECT_EQ(tasks.GetGroupStatus(group),
                  failure ? NCommon::ETaskGroupStatus::FAILED : NCommon::ETaskGroupStatus::CANCELLED);
        EXPECT_FALSE(output->TryAcquire(tasks));
        EXPECT_NO_THROW(forest.Owner.CreateEntity());
    }
}

TEST(AdaptiveTraversal, InvalidRootsAndCostsRejectBeforeCallbacksAndEmptyForestCompletes) {
    Forest forest{3, EShape::BAMBOO};
    NCommon::TaskSystem tasks{1};
    AdaptiveBatchPlanner planner{tasks};
    for (const auto& roots: {std::vector<EntityReference>{forest.Entities[0], forest.Entities[1]},
                             std::vector<EntityReference>{forest.Entities[0], forest.Entities[0]},
                             std::vector<EntityReference>{{99, forest.Entities[0].Identity}}}) {
        auto operation = Operation();
        operation.Visit = [](auto) {
            ADD_FAILURE();
            return std::uint64_t{0};
        };
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
            (void)planner.ScheduleTraversal(forest.Owner.AcquireTopologySnapshot(),
                                            roots,
                                            ETraversalMode::TOP_DOWN,
                                            operation);
        });
    }
    auto invalid = Operation();
    invalid.EstimateWork = [](auto) { return -1.0; };
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)planner.ScheduleTraversal(forest.Owner.AcquireTopologySnapshot(),
                                        forest.Roots,
                                        ETraversalMode::TOP_DOWN,
                                        invalid);
    });
    auto empty = Operation();
    const auto output = empty.Output;
    const auto group = planner.ScheduleTraversal(forest.Owner.AcquireTopologySnapshot(),
                                                 {},
                                                 ETraversalMode::TOP_DOWN,
                                                 std::move(empty));
    tasks.WaitGroup(group);
    EXPECT_TRUE(output->TryAcquire(tasks));
    EXPECT_EQ(output->GetMetrics().Packets, 0U);
}

TEST(AdaptiveTraversal, RootForestAndSelectedSubtreeKeepTheirOwnDependencyBoundary) {
    Forest forest{30, EShape::STAR};
    forest.Roots.assign(forest.Entities.begin() + 1, forest.Entities.end());
    NCommon::TaskSystem tasks{4};
    AdaptiveBatchPlanner planner{tasks};
    for (const auto mode: {ETraversalMode::TOP_DOWN, ETraversalMode::BOTTOM_UP, ETraversalMode::INDEPENDENT}) {
        auto operation = Operation();
        const auto output = operation.Output;
        operation.Visit = [](TraversalInput<std::uint64_t> input) {
            EXPECT_EQ(input.ParentResult, nullptr);
            EXPECT_TRUE(input.ChildResults.empty());
            return static_cast<std::uint64_t>(input.Entity.Identity.Index);
        };
        const auto group = planner.ScheduleTraversal(forest.Owner.AcquireTopologySnapshot(),
                                                     forest.Roots,
                                                     mode,
                                                     std::move(operation),
                                                     BatchPolicy{.MinimumPacketTime = std::chrono::nanoseconds{0}});
        tasks.WaitGroup(group);
        const auto result = output->TryAcquire(tasks);
        ASSERT_TRUE(result);
        for (const auto entity: forest.Roots) {
            EXPECT_EQ(result->Get(entity), entity.Identity.Index);
        }
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)result->Get(forest.Entities[0]); });
        EXPECT_EQ(output->GetMetrics().VisitedNodes, 29U);
        EXPECT_LE(output->GetMetrics().PeakActivePackets, 8U);
    }
}

TEST(AdaptiveTraversal, RetiredResultsDoNotAliasRecycledEntitiesAndOutputCannotBeRebound) {
    World world;
    const auto root = world.GetEntityReference(world.CreateEntity());
    NCommon::TaskSystem tasks{1};
    AdaptiveBatchPlanner planner{tasks};
    auto operation = Operation();
    const auto output = operation.Output;
    const auto group = planner.ScheduleTraversal(world.AcquireTopologySnapshot(),
                                                 std::array{root},
                                                 ETraversalMode::TOP_DOWN,
                                                 operation);
    tasks.WaitGroup(group);
    const auto result = output->TryAcquire(tasks);
    ASSERT_TRUE(result);
    world.DestroyEntity(root.Identity);
    const auto recycled = world.GetEntityReference(world.CreateEntity());
    EXPECT_EQ(recycled.Identity.Index, root.Identity.Index);
    EXPECT_NO_THROW((void)result->Get(root));
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)result->Get(recycled); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)planner.ScheduleTraversal(world.AcquireTopologySnapshot(),
                                        std::array{root},
                                        ETraversalMode::TOP_DOWN,
                                        Operation());
    });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] {
        (void)planner.ScheduleTraversal(world.AcquireTopologySnapshot(),
                                        std::array{recycled},
                                        ETraversalMode::TOP_DOWN,
                                        operation);
    });
}
} // namespace
