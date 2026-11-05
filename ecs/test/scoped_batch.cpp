#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <thread>

#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <ecs/internal/scoped_batch.h>
#include <ecs/internal/system_run.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {
using NEcs::Entity;
using NEcs::EntityReference;
using NEcs::Read;
using NEcs::SystemSchedule;
using NEcs::World;
using NEcs::Write;
using NEcs::NInternal::BatchScope;
using NEcs::NInternal::ESystemRunStatus;
using NEcs::NInternal::PrepareSystems;
using NEcs::NInternal::ScopedBatchContext;
using NEcs::NInternal::ScopedRead;
using NEcs::NInternal::ScopedView;
using NEcs::NInternal::ScopedWorldAccess;
using NEcs::NInternal::ScopedWrite;
using NEcs::NInternal::SystemRunOutcome;
using namespace std::chrono_literals;
struct Position {
    int Value = 0;
};
struct Velocity {
    int Value = 0;
};

class Rendezvous {
public:
    void Meet() {
        std::unique_lock lock{m_mutex};
        ++m_arrived;
        m_changed.notify_all();
        EXPECT_TRUE(m_changed.wait_for(lock, 5s, [&] { return m_arrived == 2; }));
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    int m_arrived = 0;
};

TEST(EcsScoped, DisjointWritersOfSameTypeRunTogether) {
    World world;
    NCommon::TaskSystem tasks{2};
    Rendezvous rendezvous;
    for (std::uint64_t id = 1; id <= 2; ++id) {
        const auto entity = world.CreateEntity();
        world.AddComponent<Position>(entity);
        const auto ref = world.GetEntityReference(entity);
        ScopedWorldAccess::Register(
                world,
                {ScopedWrite<Position>(BatchScope::Entity(ref))},
                [&, ref](ScopedBatchContext& context) {
                    rendezvous.Meet();
                    ++context.Write<Position>().Get(ref).Value;
                },
                SystemSchedule{.Id = id});
    }
    world.RunSystems(tasks);
    world.Query<Position>([](Entity, const Position& p) { EXPECT_EQ(p.Value, 1); });
    const auto dirty = ScopedWorldAccess::TakeDirtySummary(world);
    EXPECT_FALSE(dirty.AllComponents);
    EXPECT_EQ(dirty.Types, (std::vector<std::type_index>{typeid(Position)}));
    EXPECT_TRUE(ScopedWorldAccess::TakeDirtySummary(world).Types.empty());
}

TEST(EcsScoped, ReadersOfSameEntityRunTogether) {
    World world;
    NCommon::TaskSystem tasks{2};
    const auto entity = world.CreateEntity();
    world.AddComponent<Position>(entity, 42);
    const auto ref = world.GetEntityReference(entity);
    Rendezvous rendezvous;
    for (std::uint64_t id = 1; id <= 2; ++id) {
        ScopedWorldAccess::Register(
                world,
                {ScopedRead<Position>(BatchScope::Entity(ref))},
                [&, ref](ScopedBatchContext& context) {
                    rendezvous.Meet();
                    EXPECT_EQ(context.Read<Position>().Get(ref).Value, 42);
                    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)context.Write<Position>(); });
                },
                SystemSchedule{.Id = id});
    }
    world.RunSystems(tasks);
    EXPECT_TRUE(ScopedWorldAccess::TakeDirtySummary(world).Types.empty());
}

TEST(EcsScoped, OverlappingEntitySetChunkAndUnknownSerialize) {
    for (const unsigned workers: {1U, 4U}) {
        World world;
        NCommon::TaskSystem tasks{workers};
        const auto entity = world.CreateEntity();
        world.AddComponent<Position>(entity);
        const auto ref = world.GetEntityReference(entity);
        std::vector<int> order;
        const std::vector<BatchScope> scopes{BatchScope::EntitySet({ref, ref}),
                                             BatchScope::Chunk(entity.Index, 1),
                                             BatchScope::Unknown(),
                                             BatchScope::Entity(ref)};
        for (std::size_t index = 0; index < scopes.size(); ++index) {
            ScopedWorldAccess::Register(
                    world,
                    {ScopedWrite<Position>(scopes[index])},
                    [&, index, ref](ScopedBatchContext& context) {
                        EXPECT_EQ(context.Read<Position>().Get(ref).Value, static_cast<int>(index));
                        context.Write<Position>().ForEach([](EntityReference, Position& p) { ++p.Value; });
                        order.push_back(static_cast<int>(index));
                    },
                    SystemSchedule{.Id = index + 1});
        }
        world.RunSystems(tasks);
        EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3}));
        EXPECT_EQ(world.GetComponent<Position>(entity).Value, 4);
    }
}

TEST(EcsScoped, ExplicitOrderingAndLegacyWholeStorageArePreserved) {
    for (const bool serial: {false, true}) {
        World world;
        NCommon::TaskSystem tasks{3};
        const auto entity = world.CreateEntity();
        world.AddComponent<Position>(entity);
        const auto ref = world.GetEntityReference(entity);
        std::vector<int> order;
        ScopedWorldAccess::Register(
                world,
                {ScopedWrite<Position>(BatchScope::Entity(ref))},
                [&](ScopedBatchContext& context) {
                    EXPECT_EQ(context.Read<Position>().Get(ref).Value, 1);
                    context.Write<Position>().Get(ref).Value = 2;
                    order.push_back(2);
                },
                SystemSchedule{.Id = 1, .After = {9}});
        world.RegisterSystem(
                {Write<Position>()},
                [&](World& w) {
                    w.GetComponent<Position>(entity).Value = 1;
                    order.push_back(1);
                },
                SystemSchedule{.Id = 9});
        world.RegisterSystem(
                {Read<Position>()},
                [&](World& w) {
                    EXPECT_EQ(w.GetComponent<Position>(entity).Value, 2);
                    order.push_back(3);
                },
                SystemSchedule{.Id = 10});
        if (serial) {
            world.RunSystems();
        } else {
            world.RunSystems(tasks);
        }
        EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
        EXPECT_EQ(ScopedWorldAccess::TakeDirtySummary(world).Types.size(), 1U);
    }
}

TEST(EcsScoped, UndeclaredLegacySystemIsWorldBarrier) {
    World world;
    NCommon::TaskSystem tasks{3};
    std::atomic<int> phase = 0;
    ScopedWorldAccess::Register(world, {}, [&](ScopedBatchContext&) { phase = 1; }, SystemSchedule{.Id = 1});
    world.RegisterSystem(
            {},
            [&](World&) {
                EXPECT_EQ(phase.load(), 1);
                phase = 2;
            },
            SystemSchedule{.Id = 2});
    ScopedWorldAccess::Register(
            world,
            {},
            [&](ScopedBatchContext&) { EXPECT_EQ(phase.load(), 2); },
            SystemSchedule{.Id = 3});
    world.RunSystems(tasks);
    EXPECT_TRUE(ScopedWorldAccess::TakeDirtySummary(world).AllComponents);
}

TEST(EcsScoped, ViewsEnforceTypeEntityModeThreadAndLifetime) {
    World world;
    NCommon::TaskSystem tasks{1};
    const auto a = world.CreateEntity();
    const auto b = world.CreateEntity();
    world.AddComponent<Position>(a);
    world.AddComponent<Position>(b);
    const auto ref = world.GetEntityReference(a);
    std::optional<ScopedView<Position, true>> saved;
    ScopedWorldAccess::Register(
            world,
            {ScopedWrite<Position>(BatchScope::Entity(ref))},
            [&](ScopedBatchContext& context) {
                saved = context.Write<Position>();
                NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                                   [&] { (void)saved->Get(world.GetEntityReference(b)); });
                NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)context.Read<Velocity>(); });
                NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)world.GetComponent<Position>(a); });
                NTest::ExpectError(NCommon::EError::INVALID_STATE,
                                   [&] { (void)std::as_const(world).GetComponent<Position>(a); });
                NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.CreateEntity(); });
                std::thread other{
                        [&] { NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)saved->Get(ref); }); }};
                other.join();
                ++saved->Get(ref).Value;
            },
            SystemSchedule{.Id = 1});
    world.RunSystems(tasks);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)saved->Get(ref); });
    EXPECT_EQ(world.GetComponent<Position>(a).Value, 1);
}

TEST(EcsScoped, AllIdentitiesValidateBeforeAnyPublicationAndRecycleDoesNotAlias) {
    for (const bool foreign: {false, true}) {
        World world;
        World other;
        NCommon::TaskSystem tasks{2};
        auto entity = world.CreateEntity();
        const auto stale = world.GetEntityReference(entity);
        world.DestroyEntity(entity);
        entity = world.CreateEntity();
        world.AddComponent<Position>(entity);
        const auto reference = foreign ? other.GetEntityReference(other.CreateEntity()) : stale;
        int calls = 0;
        ScopedWorldAccess::Register(world, {}, [&](ScopedBatchContext&) { ++calls; }, SystemSchedule{.Id = 1});
        ScopedWorldAccess::Register(
                world,
                {ScopedWrite<Position>(BatchScope::Entity(reference))},
                [&](ScopedBatchContext&) { ++calls; },
                SystemSchedule{.Id = 2});
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { world.RunSystems(tasks); });
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(world.GetComponent<Position>(entity).Value, 0);
        EXPECT_NO_THROW(world.CreateEntity());
    }
}

TEST(EcsScoped, DeferredStructuralChangesWaitForEveryScopeAndSingleWorker) {
    for (const unsigned workers: {1U, 3U}) {
        World world;
        NCommon::TaskSystem tasks{workers};
        const auto a = world.CreateEntity();
        const auto b = world.CreateEntity();
        world.AddComponent<Position>(a);
        world.AddComponent<Velocity>(b);
        const auto ar = world.GetEntityReference(a);
        const auto br = world.GetEntityReference(b);
        ScopedWorldAccess::Register(
                world,
                {ScopedRead<Position>(BatchScope::Entity(ar))},
                [&](ScopedBatchContext& context) { context.DeferDestroy(br); },
                SystemSchedule{.Id = 1},
                10);
        ScopedWorldAccess::Register(
                world,
                {ScopedWrite<Velocity>(BatchScope::Entity(br))},
                [&](ScopedBatchContext& context) { ++context.Write<Velocity>().Get(br).Value; },
                SystemSchedule{.Id = 2});
        world.RunSystems(tasks);
        EXPECT_TRUE(world.IsAlive(a));
        EXPECT_FALSE(world.IsAlive(b));
    }
}

TEST(EcsScoped, FailedWritesStayDirtyAndDeferredCommandsAreDiscarded) {
    for (const bool serial: {false, true}) {
        World world;
        NCommon::TaskSystem tasks{1};
        const auto entity = world.CreateEntity();
        world.AddComponent<Position>(entity);
        const auto ref = world.GetEntityReference(entity);
        ScopedWorldAccess::Register(
                world,
                {ScopedWrite<Position>(BatchScope::Entity(ref))},
                [&](ScopedBatchContext& context) {
                    context.Write<Position>().Get(ref).Value = 5;
                    context.DeferDestroy(ref);
                    throw std::runtime_error{"failure"};
                },
                SystemSchedule{.Id = 1},
                10);
        if (serial) {
            EXPECT_THROW(world.RunSystems(), std::runtime_error);
        } else {
            EXPECT_THROW(world.RunSystems(tasks), std::runtime_error);
        }
        EXPECT_TRUE(world.IsAlive(entity));
        EXPECT_EQ(world.GetComponent<Position>(entity).Value, 5);
        EXPECT_EQ(ScopedWorldAccess::TakeDirtySummary(world).Types, (std::vector<std::type_index>{typeid(Position)}));
    }
}

TEST(EcsScoped, AsyncLeaseAndCancellationRetireBeforeTerminal) {
    NCommon::TaskSystem tasks{1};
    auto world = std::make_shared<World>();
    auto weak = std::weak_ptr<World>{world};
    const auto entity = world->CreateEntity();
    world->AddComponent<Position>(entity);
    const auto ref = world->GetEntityReference(entity);
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    ScopedWorldAccess::Register(
            *world,
            {ScopedWrite<Position>(BatchScope::Entity(ref))},
            [&](ScopedBatchContext& context) {
                entered.release();
                release.acquire();
                ++context.Write<Position>().Get(ref).Value;
            },
            SystemSchedule{.Id = 1});
    std::promise<SystemRunOutcome> finished;
    auto outcome = finished.get_future();
    auto prepared = PrepareSystems(world, tasks, [&](const SystemRunOutcome& result) {
        EXPECT_TRUE(weak.expired());
        finished.set_value(result);
    });
    const auto handle = prepared.GetHandle();
    prepared.Start();
    ASSERT_TRUE(entered.try_acquire_for(5s));
    world.reset();
    handle.RequestCancel();
    EXPECT_FALSE(handle.IsTerminal());
    release.release();
    ASSERT_EQ(outcome.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(outcome.get().Status, ESystemRunStatus::CANCELLED);
    EXPECT_TRUE(handle.IsTerminal());
}

TEST(EcsScoped, PreparedRunCanBeAbandonedOnWorkerWithoutWaiting) {
    NCommon::TaskSystem tasks{1};
    auto world = std::make_shared<World>();
    ScopedWorldAccess::Register(*world, {}, [](ScopedBatchContext&) { FAIL(); }, SystemSchedule{.Id = 1});
    std::promise<void> finished;
    auto done = finished.get_future();
    (void)tasks.Submit([&](NCommon::TaskContext&) {
        {
            auto prepared = PrepareSystems(world, tasks, [](const SystemRunOutcome& result) {
                EXPECT_EQ(result.Status, ESystemRunStatus::CANCELLED);
            });
        }
        finished.set_value();
    });
    ASSERT_EQ(done.wait_for(5s), std::future_status::ready);
    EXPECT_NO_THROW(world->CreateEntity());
}

TEST(EcsScoped, MultiTypeFootprintsDoNotRestoreCoarseEdges) {
    World world;
    NCommon::TaskSystem tasks{2};
    Rendezvous rendezvous;
    const auto a = world.CreateEntity();
    const auto b = world.CreateEntity();
    world.AddComponent<Position>(a);
    world.AddComponent<Position>(b);
    world.AddComponent<Velocity>(a);
    world.AddComponent<Velocity>(b);
    const auto ar = world.GetEntityReference(a);
    const auto br = world.GetEntityReference(b);
    ScopedWorldAccess::Register(
            world,
            {ScopedWrite<Position>(BatchScope::Entity(ar)), ScopedRead<Velocity>(BatchScope::Entity(br))},
            [&](ScopedBatchContext& context) {
                rendezvous.Meet();
                context.Write<Position>().Get(ar).Value = context.Read<Velocity>().Get(br).Value + 1;
            },
            SystemSchedule{.Id = 1});
    ScopedWorldAccess::Register(
            world,
            {ScopedRead<Position>(BatchScope::Entity(br)), ScopedWrite<Velocity>(BatchScope::Entity(ar))},
            [&](ScopedBatchContext& context) {
                rendezvous.Meet();
                context.Write<Velocity>().Get(ar).Value = context.Read<Position>().Get(br).Value + 2;
            },
            SystemSchedule{.Id = 2});
    world.RunSystems(tasks);
    EXPECT_EQ(world.GetComponent<Position>(a).Value, 1);
    EXPECT_EQ(world.GetComponent<Velocity>(a).Value, 2);
    const auto dirty = ScopedWorldAccess::TakeDirtySummary(world);
    EXPECT_EQ(dirty.Types.size(), 2U);
}

TEST(EcsScoped, ReadScopeDoesNotGrantWriteAndEmptyStorageStillDeclaresItsType) {
    World world;
    NCommon::TaskSystem tasks{1};
    const auto a = world.CreateEntity();
    const auto b = world.CreateEntity();
    world.AddComponent<Position>(a);
    world.AddComponent<Position>(b);
    const auto ar = world.GetEntityReference(a);
    const auto br = world.GetEntityReference(b);
    ScopedWorldAccess::Register(
            world,
            {ScopedRead<Position>(BatchScope::Entity(ar)),
             ScopedWrite<Position>(BatchScope::Entity(br)),
             ScopedRead<Velocity>()},
            [&](ScopedBatchContext& context) {
                NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)context.Write<Position>().Get(ar); });
                int reads = 0;
                int writes = 0;
                context.Read<Position>().ForEach([&](auto, const auto&) { ++reads; });
                context.Write<Position>().ForEach([&](auto, auto&) { ++writes; });
                context.Read<Velocity>().ForEach([](auto, const auto&) { FAIL(); });
                EXPECT_EQ(reads, 2);
                EXPECT_EQ(writes, 1);
            },
            SystemSchedule{.Id = 1});
    world.RunSystems(tasks);
}

TEST(EcsScoped, ConflictingWriterWaitsWhileIndependentReadyWorkMakesProgress) {
    World world;
    NCommon::TaskSystem tasks{2};
    const auto entity = world.CreateEntity();
    world.AddComponent<Position>(entity);
    const auto ref = world.GetEntityReference(entity);
    std::binary_semaphore release{0};
    std::promise<void> independent;
    auto ready = independent.get_future();
    std::atomic<int> phase = 0;
    ScopedWorldAccess::Register(
            world,
            {ScopedWrite<Position>(BatchScope::Entity(ref))},
            [&](ScopedBatchContext&) {
                phase = 1;
                release.acquire();
                phase = 2;
            },
            SystemSchedule{.Id = 1});
    ScopedWorldAccess::Register(
            world,
            {ScopedWrite<Position>(BatchScope::Entity(ref))},
            [&](ScopedBatchContext&) {
                EXPECT_EQ(phase.load(), 2);
                phase = 3;
            },
            SystemSchedule{.Id = 2});
    ScopedWorldAccess::Register(
            world,
            {},
            [&](ScopedBatchContext&) { independent.set_value(); },
            SystemSchedule{.Id = 3});
    auto run = std::async(std::launch::async, [&] { world.RunSystems(tasks); });
    EXPECT_EQ(ready.wait_for(5s), std::future_status::ready);
    EXPECT_LT(phase.load(), 2);
    release.release();
    run.get();
    EXPECT_EQ(phase.load(), 3);
}
} // namespace
