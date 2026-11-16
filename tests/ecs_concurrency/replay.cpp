#include "forest.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <numeric>
#include <random>
#include <semaphore>
#include <stdexcept>
#include <utility>

#include <ecs/internal/adaptive_traversal.h>
#include <ecs/internal/scoped_batch.h>
#include <ecs/internal/system_run.h>
#include <ecs/internal/transform_resolver.h>
#include <ecs/internal/world_changes.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace NEcsAcceptance {
namespace {
using NEcs::NInternal::AdaptiveBatchPlanner;
using NEcs::NInternal::BatchScope;
using NEcs::NInternal::ETraversalMode;
using NEcs::NInternal::ScopedBatchContext;
using NEcs::NInternal::ScopedWorldAccess;
using NEcs::NInternal::ScopedWrite;
using NEcs::NInternal::TraversalInput;
using NEcs::NInternal::TraversalOperation;
using NEcs::NInternal::TraversalOutput;

TraversalOperation<std::uint64_t> Operation(std::size_t cost) {
    TraversalOperation<std::uint64_t> operation;
    operation.Output = std::make_shared<TraversalOutput<std::uint64_t>>();
    operation.Visit = [cost](TraversalInput<std::uint64_t> input) {
        std::uint64_t value = (input.ParentResult ? *input.ParentResult : 17) * 31 + input.Entity.Identity.Index;
        for (const std::uint64_t* child: input.ChildResults) {
            value = value * 31 + *child;
        }
        return Work(value, cost);
    };
    return operation;
}

TEST(EcsAcceptance, IndependentReferenceAcrossShapesWorkersAndTraversalModes) {
    for (const EShape shape: SHAPES) {
        NEcs::World world;
        Forest forest{world, 1024, shape};
        const std::array roots{forest.Entities.front()};
        for (const unsigned workers: WORKERS) {
            SCOPED_TRACE(testing::Message() << "shape=" << static_cast<int>(shape) << " workers=" << workers);
            NCommon::TaskSystem tasks{workers};
            AdaptiveBatchPlanner planner{tasks};
            for (const ETraversalMode mode:
                 {ETraversalMode::TOP_DOWN, ETraversalMode::BOTTOM_UP, ETraversalMode::INDEPENDENT}) {
                for (const std::size_t cost: {0U, 64U}) {
                    const auto expected = forest.TraverseReference(mode == ETraversalMode::BOTTOM_UP,
                                                                   mode == ETraversalMode::INDEPENDENT,
                                                                   cost);
                    auto operation = Operation(cost);
                    auto visits = std::make_unique<std::atomic_uint[]>(forest.Entities.size());
                    const auto visit = operation.Visit;
                    operation.Visit = [&, visit](auto input) {
                        visits[input.Entity.Identity.Index].fetch_add(1);
                        return visit(input);
                    };
                    const auto output = operation.Output;
                    const auto group = planner.ScheduleTraversal(world.AcquireTopologySnapshot(),
                                                                 roots,
                                                                 mode,
                                                                 std::move(operation));
                    tasks.WaitGroup(group);
                    ASSERT_EQ(tasks.GetGroupStatus(group), NCommon::ETaskGroupStatus::COMPLETED);
                    const auto actual = output->TryAcquire(tasks);
                    ASSERT_TRUE(actual);
                    for (std::size_t index = 0; index < forest.Entities.size(); ++index) {
                        ASSERT_EQ(actual->Get(forest.Entities[index]), expected[index]) << index;
                        ASSERT_EQ(visits[forest.Entities[index].Identity.Index].load(), 1U);
                    }
                    const auto metrics = output->GetMetrics();
                    EXPECT_EQ(metrics.VisitedNodes, forest.Entities.size());
                    EXPECT_LE(metrics.PeakActivePackets, workers * 2U);
                    if (shape == EShape::BAMBOO || workers == 1) {
                        EXPECT_EQ(metrics.Packets, 1U);
                    }
                }
            }
        }
    }
}

struct Value {
    std::uint64_t Data = 1;
};

TEST(EcsAcceptance, RandomScopedReplayVersionsReparentAndLazyMatricesMatchIndependentModel) {
    for (const EShape shape: SHAPES) {
        for (const unsigned workers: WORKERS) {
            SCOPED_TRACE(testing::Message() << "shape=" << static_cast<int>(shape) << " workers=" << workers);
            NEcs::World world;
            Forest forest{world, 128, shape};
            NCommon::TaskSystem tasks{workers};
            NEcs::NInternal::TransformResolver resolver;
            std::vector<std::uint64_t> expected(forest.Entities.size(), 1);
            for (const auto entity: forest.Entities) {
                world.AddComponent<Value>(entity.Identity);
            }
            const auto domain = NEcs::NInternal::WorldChangeAccess::Domain<Value>(world, true);
            (void)NEcs::NInternal::WorldChangeAccess::PublishEpoch(world);
            // NOLINTNEXTLINE(cert-msc51-cpp): Stable seed permits exact cross-worker replay.
            std::mt19937 random{4501};
            struct Mutation {
                std::size_t Entity;
                std::uint64_t Salt;
            };
            std::vector<Mutation> mutations;
            for (std::uint64_t key = 1; key <= 64; ++key) {
                mutations.push_back({random() % expected.size(), key});
            }
            std::vector<std::size_t> registration(mutations.size());
            std::iota(registration.begin(), registration.end(), 0);
            std::shuffle(registration.begin(), registration.end(), random);
            for (const std::size_t index: registration) {
                const auto mutation = mutations[index];
                const auto entity = forest.Entities[mutation.Entity];
                ScopedWorldAccess::Register(
                        world,
                        {ScopedWrite<Value>(BatchScope::Entity(entity))},
                        [entity, mutation](ScopedBatchContext& context) {
                            auto& value = context.Write<Value>().Get(entity);
                            value.Data = value.Data * 31 + mutation.Salt;
                        },
                        NEcs::SystemSchedule{.Id = index + 1});
            }
            for (std::size_t round = 0; round < 4; ++round) {
                const auto before = NEcs::NInternal::WorldChangeAccess::CaptureSnapshot(world);
                world.RunSystems(tasks);
                for (const auto mutation: mutations) {
                    expected[mutation.Entity] = expected[mutation.Entity] * 31 + mutation.Salt;
                }
                const auto after = NEcs::NInternal::WorldChangeAccess::CaptureSnapshot(world);
                const auto dirty = after.CaptureChanges(before.GetCursor(), domain);
                EXPECT_TRUE(dirty.HasChanges());
                for (std::size_t index = 0; index < expected.size(); ++index) {
                    EXPECT_EQ(std::as_const(world).GetComponent<Value>(forest.Entities[index].Identity).Data,
                              expected[index]);
                }
                for (std::size_t index = round; index < forest.Entities.size(); index += 13) {
                    Matrix local = Identity();
                    local[3] = static_cast<float>((index + round) % 9);
                    local[1] = index % 2 == 0 ? 0.125F : 0;
                    forest.SetLocal(index, local);
                }
                // Moving a leaf to an earlier node cannot form a cycle in any generated shape.
                forest.Reparent(forest.Entities.size() - 1, round);
                const auto reference = forest.ResolveReference();
                const auto resolve = resolver.ScheduleResolve(world.AcquireTopologySnapshot(), forest.Entities, tasks);
                tasks.Wait(resolve.GetCompletion());
                ASSERT_EQ(tasks.GetStatus(resolve.GetCompletion()), NCommon::ETaskStatus::COMPLETED);
                const auto actual = resolve.ReadResolved(tasks);
                for (std::size_t index = 0; index < reference.size(); ++index) {
                    const auto& matrix = actual.Get(forest.Entities[index]).Value;
                    for (std::size_t row = 0; row < 4; ++row) {
                        for (std::size_t column = 0; column < 4; ++column) {
                            EXPECT_FLOAT_EQ(matrix(row, column), reference[index][row * 4 + column]);
                        }
                    }
                }
                const auto hot = resolver.ScheduleResolve(world.AcquireTopologySnapshot(), forest.Entities, tasks);
                tasks.Wait(hot.GetCompletion());
                EXPECT_EQ(hot.ReadResolved(tasks).GetRecomputedCount(), 0U);
                // Returned values remain immutable while the next generation of locals changes.
                const auto oldRoot = actual.Get(forest.Entities.front()).Value;
                Matrix changed = Identity();
                changed[3] = static_cast<float>(round + 11);
                forest.SetLocal(0, changed);
                EXPECT_EQ(actual.Get(forest.Entities.front()).Value.GetValues(), oldRoot.GetValues());
            }
            const auto stale = forest.Entities.back();
            world.DestroyEntity(stale.Identity);
            const auto replacement = world.GetEntityReference(world.CreateEntity());
            EXPECT_EQ(replacement.Identity.Index, stale.Identity.Index);
            EXPECT_NE(replacement, stale);
            const auto snapshot = world.AcquireTopologySnapshot();
            EXPECT_FALSE(snapshot.IsAlive(stale));
            EXPECT_TRUE(snapshot.IsAlive(replacement));
        }
    }
}

TEST(EcsAcceptance, ConflictDiagnosticsCancellationAndOwnerIsolationMatchReplay) {
    for (const unsigned workers: WORKERS) {
        NCommon::TaskSystem tasks{workers};
        NEcs::NInternal::AccessCoordinator coordinator{tasks};
        auto values = std::make_shared<std::array<std::uint64_t, 16>>();
        auto other = std::make_shared<std::array<std::uint64_t, 16>>();
        const NEcs::NInternal::DataDomainKey domain{1, 1, 1};
        coordinator.RegisterDomain(domain, values);
        coordinator.RegisterDomain({2, 1, 1}, other);
        std::binary_semaphore entered{0};
        std::binary_semaphore release{0};
        NEcs::NInternal::AccessFootprint exclusive;
        exclusive.Domain = domain;
        exclusive.Requirement = NEcs::NInternal::EAccessRequirement::EXCLUSIVE;
        (void)coordinator.Schedule({exclusive}, [&] {
            entered.release();
            // A bounded test barrier controls admission; the engine never waits on a worker join.
            if (!release.try_acquire_for(std::chrono::seconds{10})) {
                throw std::runtime_error{"acceptance admission barrier timed out"};
            }
        });
        const bool admitted = entered.try_acquire_for(std::chrono::seconds{10});
        EXPECT_TRUE(admitted);
        std::array<std::uint64_t, 16> expected{};
        std::size_t conflicts = 0;
        for (std::uint64_t salt = 1; salt <= 64; ++salt) {
            const std::size_t index = static_cast<std::size_t>(salt % expected.size());
            NEcs::NInternal::AccessFootprint footprint;
            footprint.Domain = domain;
            footprint.Requirement = NEcs::NInternal::EAccessRequirement::WRITE;
            footprint.Scope.Kind = NEcs::NInternal::EAccessScope::ENTITY;
            footprint.Scope.Begin = index;
            const auto operation = coordinator.Schedule({footprint}, [values, index, salt] {
                (*values)[index] = (*values)[index] * 31 + salt;
            });
            const auto reasons = coordinator.GetConflicts(operation);
            EXPECT_FALSE(reasons.empty());
            conflicts += reasons.size();
            for (const auto& reason: reasons) {
                EXPECT_EQ(reason.Domain, domain);
                EXPECT_LT(reason.Predecessor.GetSequence(), operation.GetSequence());
            }
            if (salt == 64) {
                EXPECT_TRUE(coordinator.Cancel(operation));
            } else {
                expected[index] = expected[index] * 31 + salt;
            }
        }
        NEcs::NInternal::AccessFootprint isolated;
        isolated.Domain = {2, 1, 1};
        isolated.Requirement = NEcs::NInternal::EAccessRequirement::WRITE;
        const auto independent = coordinator.Schedule({isolated}, [other] { (*other)[0] = 17; });
        EXPECT_TRUE(coordinator.GetConflicts(independent).empty());
        std::promise<void> drained;
        auto completion = drained.get_future();
        coordinator.Seal([&] { drained.set_value(); });
        release.release();
        ASSERT_EQ(completion.wait_for(std::chrono::seconds{10}), std::future_status::ready);
        tasks.WaitIdle();
        EXPECT_EQ(*values, expected);
        EXPECT_EQ((*other)[0], 17U);
        EXPECT_GT(conflicts, 64U);
        RecordProperty("conflict_edges", static_cast<int>(conflicts));
    }
}

TEST(EcsAcceptance, FailedAndCancelledScopedRunsDrainThenResolveAndPublishNoFalseSuccess) {
    for (const unsigned workers: WORKERS) {
        for (const bool cancel: {false, true}) {
            NCommon::TaskSystem tasks{workers};
            auto world = std::make_shared<NEcs::World>();
            Forest forest{*world, 64, EShape::BALANCED};
            const auto entity = forest.Entities.front();
            world->AddComponent<Value>(entity.Identity);
            NEcs::NInternal::SystemRun run;
            ScopedWorldAccess::Register(
                    *world,
                    {ScopedWrite<Value>(BatchScope::Entity(entity))},
                    [&](ScopedBatchContext& context) {
                        context.Write<Value>().Get(entity).Data = 19;
                        if (cancel) {
                            run.RequestCancel();
                        } else {
                            throw std::runtime_error{"acceptance failure after a dirty write"};
                        }
                    },
                    NEcs::SystemSchedule{.Id = 1});
            std::promise<NEcs::NInternal::SystemRunOutcome> outcome;
            auto completed = outcome.get_future();
            auto prepared = NEcs::NInternal::PrepareSystems(world, tasks, [&](auto value) {
                outcome.set_value(std::move(value));
            });
            run = prepared.GetHandle();
            prepared.Start();
            ASSERT_EQ(completed.wait_for(std::chrono::seconds{10}), std::future_status::ready);
            const auto result = completed.get();
            EXPECT_EQ(result.Status,
                      cancel ? NEcs::NInternal::ESystemRunStatus::CANCELLED
                             : NEcs::NInternal::ESystemRunStatus::FAILED);
            tasks.WaitIdle();
            EXPECT_EQ(std::as_const(*world).GetComponent<Value>(entity.Identity).Data, 19U);
            EXPECT_FALSE(ScopedWorldAccess::TakeDirtySummary(*world).Types.empty());
            Matrix local = Identity();
            local[3] = 9;
            forest.SetLocal(0, local);
            NEcs::NInternal::TransformResolver resolver;
            const auto resolve = resolver.ScheduleResolve(world->AcquireTopologySnapshot(), forest.Entities, tasks);
            tasks.Wait(resolve.GetCompletion());
            EXPECT_FLOAT_EQ(resolve.ReadResolved(tasks).Get(forest.Entities.back()).Value(0, 3), 9);
        }
    }
}

} // namespace
} // namespace NEcsAcceptance
