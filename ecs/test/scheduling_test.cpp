#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {

struct Position {
    int Value = 0;
};

struct Velocity {
    int Value = 0;
};

TEST(EcsScheduling, StableIdsDetermineSerialScheduleIndependentlyOfRegistration) {
    auto replay = [](bool reverseRegistration) {
        NEcs::World world;
        std::vector<int> order;
        auto low = [&] {
            world.RegisterSystem({}, [&](NEcs::World&) { order.push_back(10); }, NEcs::SystemSchedule{.Id = 10});
        };
        auto high = [&] {
            world.RegisterSystem({}, [&](NEcs::World&) { order.push_back(20); }, NEcs::SystemSchedule{.Id = 20});
        };
        if (reverseRegistration) {
            high();
            low();
        } else {
            low();
            high();
        }
        world.RunSystems();
        return order;
    };
    EXPECT_EQ(replay(false), (std::vector<int>{10, 20}));
    EXPECT_EQ(replay(false), replay(true));
}

TEST(EcsScheduling, ExplicitOrderingOverridesStableIdForConflictingAccess) {
    NEcs::World world;
    const auto entity = world.CreateEntity();
    world.AddComponent<Position>(entity);
    world.RegisterSystem(
            {NEcs::Read<Position>()},
            [&](NEcs::World& current) { EXPECT_EQ(current.GetComponent<Position>(entity).Value, 42); },
            NEcs::SystemSchedule{.Id = 10, .After = {20}});
    world.RegisterSystem(
            {NEcs::Write<Position>()},
            [&](NEcs::World& current) { current.GetComponent<Position>(entity).Value = 42; },
            NEcs::SystemSchedule{.Id = 20});
    world.RunSystems();
}

TEST(EcsScheduling, BeforeAndAfterConstraintsComposeWithoutImplicitRegistrationEdges) {
    NEcs::World world;
    std::vector<int> order;
    world.RegisterSystem({}, [&](NEcs::World&) { order.push_back(10); }, NEcs::SystemSchedule{.Id = 10, .After = {30}});
    world.RegisterSystem(
            {},
            [&](NEcs::World&) { order.push_back(20); },
            NEcs::SystemSchedule{.Id = 20, .Before = {30}});
    world.RegisterSystem({}, [&](NEcs::World&) { order.push_back(30); }, NEcs::SystemSchedule{.Id = 30});
    world.RunSystems();
    EXPECT_EQ(order, (std::vector<int>{20, 30, 10}));
}

TEST(EcsScheduling, RejectsCyclesAndUnknownIdsBeforeExecutingAnyCallback) {
    NEcs::World cyclic;
    int calls = 0;
    cyclic.RegisterSystem({}, [&](NEcs::World&) { ++calls; }, NEcs::SystemSchedule{.Id = 10, .After = {20}});
    cyclic.RegisterSystem({}, [&](NEcs::World&) { ++calls; }, NEcs::SystemSchedule{.Id = 20, .After = {10}});
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { cyclic.RunSystems(); });
    EXPECT_EQ(calls, 0);

    NEcs::World unknown;
    unknown.RegisterSystem({}, [&](NEcs::World&) { ++calls; }, NEcs::SystemSchedule{.Id = 10, .Before = {20}});
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { unknown.RunSystems(); });
    EXPECT_EQ(calls, 0);
}

TEST(EcsScheduling, RejectsDuplicateSchedulingIdsAndMixedRegistrationModes) {
    NEcs::World world;
    world.RegisterSystem({}, [](NEcs::World&) {}, NEcs::SystemSchedule{.Id = 10});
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { world.RegisterSystem({}, [](NEcs::World&) {}, NEcs::SystemSchedule{.Id = 10}); });
    EXPECT_EQ(world.GetSystemCount(), 1U);
    world.RegisterSystem({}, [](NEcs::World&) {});
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.RunSystems(); });
}

TEST(EcsScheduling, AccessConflictMatrixControlsRealWorkerOverlap) {
    for (const auto left: {NEcs::ESystemAccessMode::READ, NEcs::ESystemAccessMode::WRITE}) {
        for (const auto right: {NEcs::ESystemAccessMode::READ, NEcs::ESystemAccessMode::WRITE}) {
            NEcs::World world;
            NCommon::TaskSystem tasks{2};
            std::mutex mutex;
            std::condition_variable changed;
            bool secondStarted = false;
            bool firstFinished = false;
            bool firstSawSecond = false;
            bool secondSawFirstFinished = false;
            std::atomic_bool workersOnly = true;
            const bool independent = left == NEcs::ESystemAccessMode::READ && right == NEcs::ESystemAccessMode::READ;
            world.RegisterSystem(
                    {{.Component = typeid(Position), .Mode = right}},
                    [&](NEcs::World&) {
                        if (!NCommon::TaskSystem::IsCurrentThreadWorker()) {
                            workersOnly = false;
                        }
                        std::scoped_lock lock{mutex};
                        secondStarted = true;
                        secondSawFirstFinished = firstFinished;
                        changed.notify_all();
                    },
                    NEcs::SystemSchedule{.Id = 20});
            world.RegisterSystem(
                    {{.Component = typeid(Position), .Mode = left}},
                    [&](NEcs::World&) {
                        if (!NCommon::TaskSystem::IsCurrentThreadWorker()) {
                            workersOnly = false;
                        }
                        std::unique_lock lock{mutex};
                        firstSawSecond =
                                changed.wait_for(lock,
                                                 independent ? std::chrono::seconds{2} : std::chrono::milliseconds{20},
                                                 [&] { return secondStarted; });
                        firstFinished = true;
                    },
                    NEcs::SystemSchedule{.Id = 10});
            world.RunSystems(tasks);
            EXPECT_TRUE(workersOnly);
            EXPECT_EQ(firstSawSecond, independent);
            EXPECT_EQ(secondSawFirstFinished, !independent);
        }
    }
}

TEST(EcsScheduling, DifferentComponentWritersRunConcurrently) {
    NEcs::World world;
    NCommon::TaskSystem tasks{2};
    const auto entity = world.CreateEntity();
    world.AddComponent<Position>(entity);
    world.AddComponent<Velocity>(entity);
    std::mutex mutex;
    std::condition_variable changed;
    unsigned arrivals = 0;
    std::atomic_bool overlapped = true;
    auto arrive = [&] {
        std::unique_lock lock{mutex};
        ++arrivals;
        changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds{2}, [&] { return arrivals == 2; })) {
            overlapped = false;
        }
    };
    world.RegisterSystem(
            {NEcs::Write<Position>()},
            [&](NEcs::World& current) {
                arrive();
                current.GetComponent<Position>(entity).Value = 10;
            },
            NEcs::SystemSchedule{.Id = 10});
    world.RegisterSystem(
            {NEcs::Write<Velocity>()},
            [&](NEcs::World& current) {
                arrive();
                current.GetComponent<Velocity>(entity).Value = 20;
            },
            NEcs::SystemSchedule{.Id = 20});
    world.RunSystems(tasks);
    EXPECT_TRUE(overlapped);
    EXPECT_EQ(world.GetComponent<Position>(entity).Value, 10);
    EXPECT_EQ(world.GetComponent<Velocity>(entity).Value, 20);
}

TEST(EcsScheduling, ParallelAndSerialResultsMatchAcrossRegistrationAndWorkerCounts) {
    auto replay = [](std::size_t workerCount, bool reversed) {
        NEcs::World world;
        const auto entity = world.CreateEntity();
        world.AddComponent<Position>(entity);
        world.AddComponent<Velocity>(entity);
        int observed = 0;
        auto addWriter = [&] {
            world.RegisterSystem(
                    {NEcs::Write<Position>()},
                    [&](NEcs::World& current) { current.GetComponent<Position>(entity).Value = 4; },
                    NEcs::SystemSchedule{.Id = 10});
        };
        auto addVelocity = [&] {
            world.RegisterSystem(
                    {NEcs::Read<Position>(), NEcs::Write<Velocity>()},
                    [&](NEcs::World& current) {
                        current.GetComponent<Velocity>(entity).Value = current.GetComponent<Position>(entity).Value + 3;
                    },
                    NEcs::SystemSchedule{.Id = 20, .After = {10}});
        };
        auto addReader = [&] {
            world.RegisterSystem(
                    {NEcs::Read<Position>(), NEcs::Read<Velocity>()},
                    [&](NEcs::World& current) {
                        observed = current.GetComponent<Position>(entity).Value +
                                   current.GetComponent<Velocity>(entity).Value;
                    },
                    NEcs::SystemSchedule{.Id = 30, .After = {20}});
        };
        if (reversed) {
            addReader();
            addVelocity();
            addWriter();
        } else {
            addWriter();
            addVelocity();
            addReader();
        }
        if (workerCount == 0) {
            world.RunSystems();
        } else {
            NCommon::TaskSystem tasks{workerCount};
            world.RunSystems(tasks);
        }
        return observed;
    };
    EXPECT_EQ(replay(0, false), 11);
    for (const auto count: {1U, 2U, 4U}) {
        EXPECT_EQ(replay(count, false), replay(0, false));
        EXPECT_EQ(replay(count, true), replay(0, false));
    }
}

TEST(EcsScheduling, StructuralCommitOccursAfterJoinAndPreservesProducerIdentity) {
    auto replay = [](std::size_t workerCount, bool reverse) {
        NEcs::World world;
        world.BeginDeferredStructuralPhase({{.Order = 0, .EntityCreateCount = 1, .SystemOrder = 10},
                                            {.Order = 0, .EntityCreateCount = 1, .SystemOrder = 20}});
        NEcs::Entity low;
        NEcs::Entity high;
        std::atomic_bool invisible = true;
        auto add = [&](std::uint32_t order, NEcs::Entity& created) {
            world.RegisterSystem(
                    {},
                    [&, order](NEcs::World& current) {
                        auto producer = current.CreateDeferredStructuralCommandBuffer(0);
                        created = current.DeferCreateEntity(producer);
                        NEcs::World::DeferAddComponent<Position>(producer, created, static_cast<int>(order));
                        current.SubmitDeferredStructuralCommands(std::move(producer));
                        if (current.IsAlive(created)) {
                            invisible = false;
                        }
                    },
                    NEcs::SystemSchedule{.Id = order},
                    order);
        };
        if (reverse) {
            add(20, high);
            add(10, low);
        } else {
            add(10, low);
            add(20, high);
        }
        NCommon::TaskSystem tasks{workerCount};
        world.RunSystems(tasks);
        EXPECT_TRUE(invisible);
        EXPECT_EQ(world.GetComponent<Position>(low).Value, 10);
        EXPECT_EQ(world.GetComponent<Position>(high).Value, 20);
        return std::pair{low, high};
    };
    const auto expected = replay(1, false);
    EXPECT_EQ(replay(2, true), expected);
    EXPECT_EQ(replay(4, false), expected);
}

TEST(EcsScheduling, FailureJoinsIndependentWorkCancelsDependentsAndRollsBackPhase) {
    NEcs::World world;
    NCommon::TaskSystem tasks{3};
    const auto existing = world.CreateEntity();
    world.BeginDeferredStructuralPhase({{.Order = 0, .EntityCreateCount = 1, .SystemOrder = 10},
                                        {.Order = 0, .EntityCreateCount = 1, .SystemOrder = 30}});
    NEcs::Entity created;
    std::atomic_bool independentFinished = false;
    std::atomic_bool dependentExecuted = false;
    world.RegisterSystem(
            {},
            [&](NEcs::World& current) {
                auto producer = current.CreateDeferredStructuralCommandBuffer(0);
                created = current.DeferCreateEntity(producer);
                NEcs::World::DeferDestroyEntity(producer, existing);
                current.SubmitDeferredStructuralCommands(std::move(producer));
                independentFinished = true;
            },
            NEcs::SystemSchedule{.Id = 10},
            10);
    world.RegisterSystem(
            {},
            [](NEcs::World&) { GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "test failure"); },
            NEcs::SystemSchedule{.Id = 20});
    world.RegisterSystem(
            {},
            [&](NEcs::World&) { dependentExecuted = true; },
            NEcs::SystemSchedule{.Id = 30, .After = {20}},
            30);

    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.RunSystems(tasks); });
    EXPECT_TRUE(independentFinished);
    EXPECT_FALSE(dependentExecuted);
    EXPECT_TRUE(world.IsAlive(existing));
    EXPECT_FALSE(world.IsAlive(created));
    EXPECT_TRUE(world.IsAlive(world.CreateEntity()));
    world.ApplyDeferredStructuralChanges();
    EXPECT_TRUE(world.IsAlive(existing));
}

TEST(EcsScheduling, RejectsParallelCoordinatorOnTaskSystemWorkerBeforeSubmittingWork) {
    NEcs::World world;
    NCommon::TaskSystem tasks{1};
    std::atomic_bool invoked = false;
    world.RegisterSystem({}, [&](NEcs::World&) { invoked = true; }, NEcs::SystemSchedule{.Id = 10});
    const auto coordinator = tasks.Submit([&](NCommon::TaskContext&) {
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.RunSystems(tasks); });
    });
    tasks.Wait(coordinator);
    EXPECT_EQ(tasks.GetStatus(coordinator), NCommon::ETaskStatus::COMPLETED);
    EXPECT_FALSE(invoked);
    world.RunSystems(tasks);
    EXPECT_TRUE(invoked);
}

TEST(EcsScheduling, RepeatedConflictingUpdatesAreDeterministic) {
    for (const auto workers: {1U, 4U}) {
        NEcs::World world;
        NCommon::TaskSystem tasks{workers};
        const auto entity = world.CreateEntity();
        world.AddComponent<Position>(entity);
        for (std::uint64_t id = 12; id > 0; --id) {
            world.RegisterSystem(
                    {NEcs::Write<Position>()},
                    [&](NEcs::World& current) { ++current.GetComponent<Position>(entity).Value; },
                    NEcs::SystemSchedule{.Id = id});
        }
        for (unsigned frame = 0; frame < 100; ++frame) {
            world.RunSystems(tasks);
        }
        EXPECT_EQ(world.GetComponent<Position>(entity).Value, 1200);
    }
}

TEST(EcsScheduling, UnusedReusedReservationsDoNotExposeWorkerCompletionOrder) {
    auto replay = [](bool reverseCompletion) {
        NEcs::World world;
        const auto first = world.CreateEntity();
        const auto second = world.CreateEntity();
        world.DestroyEntity(first);
        world.DestroyEntity(second);
        world.BeginDeferredStructuralPhase({{.Order = 0, .EntityCreateCount = 1, .SystemOrder = 10},
                                            {.Order = 0, .EntityCreateCount = 1, .SystemOrder = 20}});
        std::mutex mutex;
        std::condition_variable changed;
        bool highSubmitted = false;
        world.RegisterSystem(
                {},
                [&](NEcs::World& current) {
                    auto producer = current.CreateDeferredStructuralCommandBuffer(0);
                    if (reverseCompletion) {
                        std::unique_lock lock{mutex};
                        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds{2}, [&] { return highSubmitted; }));
                    }
                    current.SubmitDeferredStructuralCommands(std::move(producer));
                },
                NEcs::SystemSchedule{.Id = 10},
                10);
        world.RegisterSystem(
                {},
                [&](NEcs::World& current) {
                    auto producer = current.CreateDeferredStructuralCommandBuffer(0);
                    current.SubmitDeferredStructuralCommands(std::move(producer));
                    std::scoped_lock lock{mutex};
                    highSubmitted = true;
                    changed.notify_all();
                },
                NEcs::SystemSchedule{.Id = 20},
                20);
        NCommon::TaskSystem tasks{reverseCompletion ? 2U : 1U};
        world.RunSystems(tasks);
        return world.CreateEntity();
    };
    EXPECT_EQ(replay(false), replay(true));
}

TEST(EcsScheduling, ExplicitTopologyDoesNotCreateHazardCycles) {
    NEcs::World world;
    NCommon::TaskSystem tasks{3};
    std::vector<int> order;
    world.RegisterSystem(
            {NEcs::Write<Position>()},
            [&](NEcs::World&) { order.push_back(1); },
            NEcs::SystemSchedule{.Id = 1, .After = {3}});
    world.RegisterSystem(
            {NEcs::Write<Position>()},
            [&](NEcs::World&) { order.push_back(2); },
            NEcs::SystemSchedule{.Id = 2});
    world.RegisterSystem(
            {NEcs::Write<Position>()},
            [&](NEcs::World&) { order.push_back(3); },
            NEcs::SystemSchedule{.Id = 3});
    world.RunSystems(tasks);
    EXPECT_EQ(order, (std::vector<int>{2, 3, 1}));
}

} // namespace
