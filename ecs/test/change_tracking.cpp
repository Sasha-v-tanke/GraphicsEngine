#include <barrier>
#include <stdexcept>

#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <ecs/internal/scoped_batch.h>
#include <ecs/internal/system_run.h>
#include <ecs/internal/world_changes.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {
using NEcs::World;
using NEcs::NInternal::BatchScope;
using NEcs::NInternal::ScopedBatchContext;
using NEcs::NInternal::ScopedRead;
using NEcs::NInternal::ScopedWorldAccess;
using NEcs::NInternal::ScopedWrite;
using NEcs::NInternal::WorldChangeAccess;
struct Position {
    int Value = 0;
};
struct Velocity {
    int Value = 0;
};

TEST(EcsChanges, LegacyMutationAndConstReadsPreservePublicationBoundary) {
    World world;
    const auto entity = world.CreateEntity();
    world.AddComponent<Position>(entity);
    const auto domain = WorldChangeAccess::Domain<Position>(world);
    static_cast<void>(WorldChangeAccess::PublishEpoch(world));
    const auto before = WorldChangeAccess::CaptureSnapshot(world);
    EXPECT_EQ(std::as_const(world).GetComponent<Position>(entity).Value, 0);
    EXPECT_FALSE(WorldChangeAccess::CaptureSnapshot(world).CaptureChanges(before.GetCursor(), domain).HasChanges());
    ++world.GetComponent<Position>(entity).Value;
    EXPECT_TRUE(WorldChangeAccess::CaptureSnapshot(world).CaptureChanges(before.GetCursor(), domain).FullDirty);
    static_cast<void>(WorldChangeAccess::PublishEpoch(world));
    const auto after = WorldChangeAccess::CaptureSnapshot(world);
    EXPECT_TRUE(after.CaptureChanges(before.GetCursor(), domain).FullDirty);
    EXPECT_FALSE(after.CaptureChanges(after.GetCursor(), domain).HasChanges());
    EXPECT_FALSE(before.CaptureChanges(before.GetCursor(), domain).HasChanges());
}

TEST(EcsChanges, ConcurrentDisjointBatchesMergeRangesAtOneEpoch) {
    World world;
    NCommon::TaskSystem tasks{2};
    std::barrier rendezvous{2};
    for (std::uint64_t index = 0; index < 2; ++index) {
        const auto entity = world.CreateEntity();
        world.AddComponent<Position>(entity);
        const auto reference = world.GetEntityReference(entity);
        ScopedWorldAccess::Register(
                world,
                {ScopedWrite<Position>(BatchScope::Entity(reference))},
                [&, reference](ScopedBatchContext& context) {
                    rendezvous.arrive_and_wait();
                    ++context.Write<Position>().Get(reference).Value;
                },
                NEcs::SystemSchedule{.Id = index + 1});
    }
    const auto domain = WorldChangeAccess::Domain<Position>(world, true);
    static_cast<void>(WorldChangeAccess::PublishEpoch(world));
    const auto before = WorldChangeAccess::CaptureSnapshot(world).GetCursor();
    world.RunSystems(tasks);
    const auto after = WorldChangeAccess::CaptureSnapshot(world);
    const auto changes = after.CaptureChanges(before, domain);
    EXPECT_FALSE(changes.FullDirty);
    EXPECT_EQ(changes.Ranges, (std::vector<NCommon::DirtyRange>{{0, 2}}));
    EXPECT_EQ(changes.Entities.size(), 2U);
    EXPECT_EQ(changes.Version.Value, before.Epoch.Value + 1);
    EXPECT_EQ(after.GetEntityRevision(domain, {0, 0}), changes.Version);
    EXPECT_EQ(after.GetEntityRevision(domain, {1, 1}), changes.Version);
    EXPECT_EQ(after.GetChunkVersion(domain, 0), changes.Version);
}

TEST(EcsChanges, ReadOnlyRunProducesNoDeltaAndWorldIdentitiesStayDistinct) {
    World world;
    const auto entity = world.CreateEntity();
    world.AddComponent<Position>(entity, 42);
    const auto reference = world.GetEntityReference(entity);
    ScopedWorldAccess::Register(
            world,
            {ScopedRead<Position>(BatchScope::Entity(reference))},
            [reference](ScopedBatchContext& context) { EXPECT_EQ(context.Read<Position>().Get(reference).Value, 42); },
            NEcs::SystemSchedule{.Id = 1});
    const auto domain = WorldChangeAccess::Domain<Position>(world);
    static_cast<void>(WorldChangeAccess::PublishEpoch(world));
    const auto cursor = WorldChangeAccess::CaptureSnapshot(world).GetCursor();
    world.RunSystems();
    EXPECT_FALSE(WorldChangeAccess::CaptureSnapshot(world).CaptureChanges(cursor, domain).HasChanges());
    World other;
    EXPECT_NE(domain, WorldChangeAccess::Domain<Position>(other));
}

TEST(EcsChanges, FailedScopedTaskPublishesPartialWritesAndLeavesUnchangedStorageClean) {
    for (bool asynchronous: {false, true}) {
        World world;
        NCommon::TaskSystem tasks{2};
        const auto entity = world.CreateEntity();
        world.AddComponent<Position>(entity);
        world.AddComponent<Velocity>(entity);
        const auto reference = world.GetEntityReference(entity);
        ScopedWorldAccess::Register(
                world,
                {ScopedWrite<Position>(BatchScope::Entity(reference))},
                [reference](ScopedBatchContext& context) {
                    context.Write<Position>().Get(reference).Value = 9;
                    throw std::runtime_error("after write");
                },
                NEcs::SystemSchedule{.Id = 1});
        const auto position = WorldChangeAccess::Domain<Position>(world);
        const auto velocity = WorldChangeAccess::Domain<Velocity>(world);
        static_cast<void>(WorldChangeAccess::PublishEpoch(world));
        const auto cursor = WorldChangeAccess::CaptureSnapshot(world).GetCursor();
        if (asynchronous) {
            EXPECT_THROW(world.RunSystems(tasks), std::runtime_error);
        } else {
            EXPECT_THROW(world.RunSystems(), std::runtime_error);
        }
        const auto snapshot = WorldChangeAccess::CaptureSnapshot(world);
        EXPECT_EQ(std::as_const(world).GetComponent<Position>(entity).Value, 9);
        EXPECT_EQ(snapshot.CaptureChanges(cursor, position).Ranges, (std::vector<NCommon::DirtyRange>{{0, 1}}));
        EXPECT_FALSE(snapshot.CaptureChanges(cursor, velocity).HasChanges());
    }
}

TEST(EcsChanges, LegacyDeclaredStorageRemainsCoarseEvenAfterFailure) {
    World world;
    const auto entity = world.CreateEntity();
    world.AddComponent<Position>(entity);
    const auto domain = WorldChangeAccess::Domain<Position>(world);
    world.RegisterSystem({NEcs::Write<Position>()}, [entity](World& owner) {
        ++owner.GetComponent<Position>(entity).Value;
        throw std::runtime_error("after legacy write");
    });
    static_cast<void>(WorldChangeAccess::PublishEpoch(world));
    const auto cursor = WorldChangeAccess::CaptureSnapshot(world).GetCursor();
    EXPECT_THROW(world.RunSystems(), std::runtime_error);
    EXPECT_TRUE(WorldChangeAccess::CaptureSnapshot(world).CaptureChanges(cursor, domain).FullDirty);
}

TEST(EcsChanges, StructuralRemovalCannotLeaveAConsumerClean) {
    World world;
    const auto entity = world.CreateEntity();
    world.AddComponent<Position>(entity);
    const auto domain = WorldChangeAccess::Domain<Position>(world);
    static_cast<void>(WorldChangeAccess::PublishEpoch(world));
    const auto cursor = WorldChangeAccess::CaptureSnapshot(world).GetCursor();
    world.DestroyEntity(entity);
    static_cast<void>(WorldChangeAccess::PublishEpoch(world));
    EXPECT_TRUE(WorldChangeAccess::CaptureSnapshot(world).CaptureChanges(cursor, domain).FullDirty);
}

TEST(EcsChanges, CancellationBeforeInvocationContributesNoDirtyFootprint) {
    auto world = std::make_shared<World>();
    NCommon::TaskSystem tasks{1};
    const auto entity = world->CreateEntity();
    world->AddComponent<Position>(entity);
    const auto reference = world->GetEntityReference(entity);
    ScopedWorldAccess::Register(
            *world,
            {ScopedWrite<Position>(BatchScope::Entity(reference))},
            [](ScopedBatchContext&) { ADD_FAILURE() << "Cancelled callback ran"; },
            NEcs::SystemSchedule{.Id = 1});
    const auto domain = WorldChangeAccess::Domain<Position>(*world);
    static_cast<void>(WorldChangeAccess::PublishEpoch(*world));
    const auto cursor = WorldChangeAccess::CaptureSnapshot(*world).GetCursor();
    auto prepared = NEcs::NInternal::PrepareSystems(world, tasks, [](const auto&) {});
    const auto handle = prepared.GetHandle();
    handle.RequestCancel();
    prepared.Start();
    tasks.WaitIdle();
    ASSERT_TRUE(handle.IsTerminal());
    EXPECT_EQ(handle.TryGetOutcome()->Status, NEcs::NInternal::ESystemRunStatus::CANCELLED);
    EXPECT_FALSE(WorldChangeAccess::CaptureSnapshot(*world).CaptureChanges(cursor, domain).HasChanges());
}
} // namespace
