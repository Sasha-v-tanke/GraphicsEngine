#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>

#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <ecs/internal/system_run.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {
struct Value {
    int Number = 0;
};
struct Other {
    int Number = 0;
};
struct Explodes {
    Explodes() {
        throw std::runtime_error("component construction failed");
    }
};
using State = NEcs::World::EStructuralState;

TEST(EcsStructuralCommit, PendingChainPublishesOnlyAtCommitAndRetainsMoveOnlyPayload) {
    NEcs::World world;
    const auto entity = world.QueueCreateEntity();
    world.QueueAddComponent<Value>(entity, 42);
    world.QueueAddComponent<std::unique_ptr<int>>(entity, std::make_unique<int>(7));
    EXPECT_EQ(world.GetStructuralState(entity), State::PENDING_CREATE);
    EXPECT_FALSE(world.IsAlive(entity.Identity));
    EXPECT_FALSE(world.HasComponent<Value>(entity.Identity));
    std::size_t count = 0;
    world.Query<Value>([&](auto, auto&) { ++count; });
    EXPECT_EQ(count, 0U);
    EXPECT_THROW(static_cast<void>(world.GetComponent<Value>(entity.Identity)), NCommon::Exception);
    world.CommitPending();
    EXPECT_EQ(world.GetStructuralState(entity), State::ALIVE);
    EXPECT_EQ(world.GetComponent<Value>(entity.Identity).Number, 42);
    EXPECT_EQ(*world.GetComponent<std::unique_ptr<int>>(entity.Identity), 7);
}

TEST(EcsStructuralCommit, DuplicateAndImplicitReplacementAreRejectedAtIntake) {
    NEcs::World world;
    const auto entity = world.QueueCreateEntity();
    world.QueueAddComponent<Value>(entity, Value{3});
    NTest::ExpectError(NCommon::EError::ALREADY_EXISTS, [&] { world.QueueAddComponent<Value>(entity, 5); });
    NTest::ExpectError(NCommon::EError::ALREADY_EXISTS, [&] { world.QueueRemoveComponent<Value>(entity); });
    world.CommitPending();
    NTest::ExpectError(NCommon::EError::ALREADY_EXISTS, [&] { world.QueueAddComponent<Value>(entity, 9); });
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { world.QueueRemoveComponent<Other>(entity); });
    world.QueueRemoveComponent<Value>(entity);
    NTest::ExpectError(NCommon::EError::ALREADY_EXISTS, [&] { world.QueueAddComponent<Value>(entity, 9); });
    EXPECT_TRUE(world.HasComponent<Value>(entity.Identity));
    world.CommitPending();
    EXPECT_FALSE(world.HasComponent<Value>(entity.Identity));
    world.QueueAddComponent<Value>(entity, 9);
    world.CommitPending();
    EXPECT_EQ(world.GetComponent<Value>(entity.Identity).Number, 9);
}

TEST(EcsStructuralCommit, CrossProducerConflictRejectsWholeCommitBeforeAnyPublication) {
    for (bool reverse: {false, true}) {
        NEcs::World world;
        const auto original = world.GetEntityReference(world.CreateEntity());
        const auto pending = world.QueueCreateEntity();
        world.QueueAddComponent<Other>(pending, 11);
        auto first = world.CreateDeferredStructuralCommandBuffer(10);
        auto second = world.CreateDeferredStructuralCommandBuffer(20);
        NEcs::World::QueueAddComponent<Value>(first, original, 1);
        NEcs::World::QueueAddComponent<Value>(second, original, 2);
        if (reverse) {
            world.SubmitDeferredStructuralCommands(std::move(second));
            world.SubmitDeferredStructuralCommands(std::move(first));
        } else {
            world.SubmitDeferredStructuralCommands(std::move(first));
            world.SubmitDeferredStructuralCommands(std::move(second));
        }
        NTest::ExpectError(NCommon::EError::ALREADY_EXISTS, [&] { world.CommitPending(); });
        EXPECT_FALSE(world.IsExecutionPoisoned());
        EXPECT_EQ(world.GetAliveEntityCount(), 1U);
        EXPECT_FALSE(world.HasComponent<Value>(original.Identity));
        EXPECT_EQ(world.GetStructuralState(pending), State::DESTROYED);
        world.QueueAddComponent<Value>(original, 8);
        world.CommitPending();
        EXPECT_EQ(world.GetComponent<Value>(original.Identity).Number, 8);
    }
}

TEST(EcsStructuralCommit, ProducerOrderAndReservationsSurviveReverseSubmission) {
    NEcs::World world;
    world.BeginDeferredStructuralPhase({{.Order = 10, .EntityCreateCount = 1}, {.Order = 20, .EntityCreateCount = 1}});
    auto first = world.CreateDeferredStructuralCommandBuffer(10);
    auto second = world.CreateDeferredStructuralCommandBuffer(20);
    const auto high = world.QueueCreateEntity(second);
    const auto low = world.QueueCreateEntity(first);
    NEcs::World::QueueAddComponent<Value>(second, high, 20);
    NEcs::World::QueueAddComponent<Value>(first, low, 10);
    world.SubmitDeferredStructuralCommands(std::move(second));
    world.SubmitDeferredStructuralCommands(std::move(first));
    world.CommitPending();
    EXPECT_LT(low.Identity.Index, high.Identity.Index);
    EXPECT_EQ(world.GetComponent<Value>(low.Identity).Number, 10);
    EXPECT_EQ(world.GetComponent<Value>(high.Identity).Number, 20);
}

TEST(EcsStructuralCommit, DestroyIsDeferredIdempotentAndInvalidatesRecycledIdentity) {
    NEcs::World world;
    NEcs::World foreign;
    const auto entity = world.QueueCreateEntity();
    world.QueueAddComponent<Value>(entity, 3);
    world.CommitPending();
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { foreign.QueueDestroyEntity(entity); });
    world.QueueDestroyEntity(entity);
    world.QueueDestroyEntity(entity);
    EXPECT_EQ(world.GetStructuralState(entity), State::PENDING_DESTROY);
    EXPECT_TRUE(world.IsAlive(entity.Identity));
    world.CommitPending();
    EXPECT_EQ(world.GetStructuralState(entity), State::DESTROYED);
    const auto replacement = world.QueueCreateEntity();
    EXPECT_EQ(entity.Identity.Index, replacement.Identity.Index);
    EXPECT_NE(entity.Identity.Generation, replacement.Identity.Generation);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.QueueAddComponent<Value>(entity, 8); });
    world.QueueDestroyEntity(replacement);
    world.CommitPending();
    EXPECT_EQ(world.GetAliveEntityCount(), 0U);
}

TEST(EcsStructuralCommit, HierarchyConflictsArePreflightedAgainstShadowTopology) {
    NEcs::World world;
    const auto parent = world.QueueCreateEntity();
    const auto child = world.QueueCreateEntity();
    world.QueueAddComponent<Value>(child, 1);
    world.QueueReparent(child, parent);
    world.QueueReparent(parent, child);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { world.CommitPending(); });
    EXPECT_EQ(world.GetAliveEntityCount(), 0U);
    EXPECT_FALSE(world.IsExecutionPoisoned());
}

TEST(EcsStructuralCommit, EnabledMetadataPublishesWithoutChangingLegacyQueryContract) {
    NEcs::World world;
    const auto entity = world.QueueCreateEntity();
    world.QueueAddComponent<Value>(entity, 4);
    world.QueueSetEnabled(entity, false);
    world.QueueSetComponentEnabled<Value>(entity, false);
    world.CommitPending();
    EXPECT_FALSE(world.IsEntityEnabled(entity));
    EXPECT_FALSE(world.IsComponentEnabled<Value>(entity));
    EXPECT_TRUE(world.HasComponent<Value>(entity.Identity));
    world.QueueSetEnabled(entity, true);
    world.QueueSetComponentEnabled<Value>(entity, true);
    EXPECT_FALSE(world.IsEntityEnabled(entity));
    world.CommitPending();
    EXPECT_TRUE(world.IsEntityEnabled(entity));
    EXPECT_TRUE(world.IsComponentEnabled<Value>(entity));
}

TEST(EcsStructuralCommit, ConstructorFailurePoisonsAndNeverReportsSuccessfulPublication) {
    NEcs::World world;
    const auto entity = world.QueueCreateEntity();
    world.QueueAddComponent<Explodes>(entity);
    EXPECT_THROW(world.CommitPending(), std::runtime_error);
    EXPECT_TRUE(world.IsExecutionPoisoned());
    EXPECT_THROW(world.CommitPending(), NCommon::Exception);
    EXPECT_THROW(world.RunSystems(), NCommon::Exception);
    EXPECT_THROW(world.Query<Value>([](auto, auto&) {}), NCommon::Exception);
    EXPECT_THROW(static_cast<void>(world.AcquireTopologySnapshot()), NCommon::Exception);
    EXPECT_THROW(world.QueueDestroyEntity(entity), NCommon::Exception);
}

TEST(EcsStructuralCommit, SnapshotAndWorkerCannotExplicitlyCommit) {
    NEcs::World world;
    const auto entity = world.QueueCreateEntity();
    {
        const auto snapshot = world.AcquireTopologySnapshot();
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.CommitPending(); });
    }
    NCommon::TaskSystem tasks{1};
    const auto task = tasks.Submit([&](NCommon::TaskContext&) {
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.CommitPending(); });
    });
    tasks.Wait(task);
    world.CommitPending();
    EXPECT_TRUE(world.IsAlive(entity.Identity));
}

TEST(EcsStructuralCommit, InitializationCommitsBeforeSystemsAndSystemCommandsAtFinalization) {
    NEcs::World world;
    const auto entity = world.QueueCreateEntity();
    world.QueueAddComponent<Value>(entity, 5);
    world.RegisterSystem(
            {NEcs::Read<Value>()},
            [&](NEcs::World& current) {
                EXPECT_TRUE(current.IsAlive(entity.Identity));
                EXPECT_EQ(current.GetComponent<Value>(entity.Identity).Number, 5);
                current.QueueAddComponent<Other>(entity, 9);
                EXPECT_FALSE(current.HasComponent<Other>(entity.Identity));
                NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { current.CommitPending(); });
            },
            10);
    world.RunSystems();
    EXPECT_EQ(world.GetComponent<Other>(entity.Identity).Number, 9);
}

TEST(EcsStructuralCommit, FailedSystemDiscardsUserCommandsAndReservedCreates) {
    auto world = std::make_shared<NEcs::World>();
    const auto original = world->GetEntityReference(world->CreateEntity());
    world->BeginDeferredStructuralPhase({{.Order = 1, .EntityCreateCount = 1, .SystemOrder = 10}});
    NEcs::EntityReference pending;
    world->RegisterSystem(
            {},
            [&](NEcs::World& current) {
                auto producer = current.CreateDeferredStructuralCommandBuffer(1);
                pending = current.QueueCreateEntity(producer);
                NEcs::World::QueueAddComponent<Value>(producer, pending, 2);
                current.SubmitDeferredStructuralCommands(std::move(producer));
                current.QueueAddComponent<Other>(original, 3);
                throw std::runtime_error("failed system");
            },
            NEcs::SystemSchedule{.Id = 10},
            10);
    NCommon::TaskSystem tasks{1};
    EXPECT_THROW(world->RunSystems(tasks), std::runtime_error);
    EXPECT_EQ(world->GetStructuralState(pending), State::DESTROYED);
    EXPECT_FALSE(world->HasComponent<Other>(original.Identity));
    EXPECT_FALSE(world->IsExecutionPoisoned());
}

TEST(EcsStructuralCommit, AsyncCommitConflictProducesFailedOutcomeWithOneWorker) {
    auto world = std::make_shared<NEcs::World>();
    const auto entity = world->GetEntityReference(world->CreateEntity());
    for (std::uint32_t order: {10U, 20U}) {
        world->RegisterSystem(
                {},
                [entity, order](NEcs::World& current) {
                    current.QueueAddComponent<Value>(entity, static_cast<int>(order));
                },
                NEcs::SystemSchedule{.Id = order},
                order);
    }
    NCommon::TaskSystem tasks{1};
    NTest::ExpectError(NCommon::EError::ALREADY_EXISTS, [&] { world->RunSystems(tasks); });
    EXPECT_FALSE(world->HasComponent<Value>(entity.Identity));
}

TEST(EcsStructuralCommit, CommitRejectsLiveBufferWithoutEntityReservations) {
    NEcs::World world;
    const auto entity = world.QueueCreateEntity();
    {
        auto producer = world.CreateDeferredStructuralCommandBuffer(1);
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.CommitPending(); });
    }
    world.CommitPending();
    EXPECT_TRUE(world.IsAlive(entity.Identity));
}

struct ReentrantCommit {
    explicit ReentrantCommit(NEcs::World* world) {
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world->CommitPending(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(world->QueueCreateEntity()); });
    }
};
TEST(EcsStructuralCommit, ReentrantPublicationCannotOpenAnotherCommitOrIntake) {
    NEcs::World world;
    const auto entity = world.QueueCreateEntity();
    world.QueueAddComponent<ReentrantCommit>(entity, &world);
    world.CommitPending();
    EXPECT_FALSE(world.IsExecutionPoisoned());
    EXPECT_TRUE(world.HasComponent<ReentrantCommit>(entity.Identity));
}

TEST(EcsStructuralCommit, ConcurrentProducersOwnDisjointPendingCommands) {
    NEcs::World world;
    world.BeginDeferredStructuralPhase({{.Order = 10, .EntityCreateCount = 1}, {.Order = 20, .EntityCreateCount = 1}});
    auto first = world.CreateDeferredStructuralCommandBuffer(10);
    auto second = world.CreateDeferredStructuralCommandBuffer(20);
    NEcs::EntityReference low;
    NEcs::EntityReference high;
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    std::thread slow([&] {
        low = world.QueueCreateEntity(first);
        entered.release();
        release.acquire();
        NEcs::World::QueueAddComponent<Value>(first, low, 10);
    });
    std::thread fast([&] {
        entered.acquire();
        high = world.QueueCreateEntity(second);
        NEcs::World::QueueAddComponent<Value>(second, high, 20);
        release.release();
    });
    slow.join();
    fast.join();
    world.SubmitDeferredStructuralCommands(std::move(second));
    world.SubmitDeferredStructuralCommands(std::move(first));
    world.CommitPending();
    EXPECT_EQ(world.GetComponent<Value>(low.Identity).Number, 10);
    EXPECT_EQ(world.GetComponent<Value>(high.Identity).Number, 20);
    EXPECT_LT(low.Identity.Index, high.Identity.Index);
}

TEST(EcsStructuralCommit, CancelClosesUserProducerAdmissionAndRetiresPendingCreate) {
    auto world = std::make_shared<NEcs::World>();
    world->BeginDeferredStructuralPhase({{.Order = 1, .EntityCreateCount = 1, .SystemOrder = 10}});
    std::optional<NEcs::World::DeferredStructuralCommandBuffer> producer;
    NEcs::EntityReference pending;
    std::binary_semaphore entered{0};
    world->RegisterSystem(
            {},
            [&](NEcs::World& current) {
                producer.emplace(current.CreateDeferredStructuralCommandBuffer(1));
                pending = current.QueueCreateEntity(*producer);
                entered.release();
            },
            NEcs::SystemSchedule{.Id = 10},
            10);
    NCommon::TaskSystem tasks{1};
    auto prepared = NEcs::NInternal::PrepareSystems(world, tasks, [](const auto&) {});
    const auto run = prepared.GetHandle();
    prepared.Start();
    entered.acquire();
    run.RequestCancel();
    NTest::ExpectError(NCommon::EError::INVALID_STATE,
                       [&] { NEcs::World::QueueAddComponent<Value>(*producer, pending, 1); });
    EXPECT_FALSE(run.IsTerminal());
    producer.reset();
    tasks.WaitIdle();
    ASSERT_TRUE(run.TryGetOutcome());
    EXPECT_EQ(run.TryGetOutcome()->Status, NEcs::NInternal::ESystemRunStatus::CANCELLED);
    EXPECT_EQ(world->GetStructuralState(pending), State::DESTROYED);
    EXPECT_FALSE(world->IsExecutionPoisoned());
}
} // namespace
