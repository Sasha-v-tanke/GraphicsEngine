#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <GraphicsEngine/ecs/entity_ref.h>
#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <ecs/internal/scoped_batch.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {
struct Health {
    int Current;
    int Maximum;
    void Damage(int value) {
        Current -= value;
    }
};
struct Position {
    int Value = 0;
};
struct Missing {};

static_assert(std::is_copy_constructible_v<NEcs::EntityRef>);
static_assert(std::is_same_v<decltype(std::declval<NEcs::EntityRef>().AddComponent<Health>(1, 2)), NEcs::EntityRef>);
static_assert(
        std::is_same_v<decltype(std::declval<NEcs::EntityRef&>().AddComponent<Health>(Health{1, 2})), NEcs::EntityRef>);
static_assert(std::is_same_v<decltype(std::declval<NEcs::EntityRef&>().GetComponent<Health>()), Health&>);
static_assert(std::is_same_v<decltype(std::declval<const NEcs::EntityRef&>().GetComponent<Health>()), const Health&>);
static_assert(std::is_same_v<decltype(std::declval<NEcs::EntityRef&>().TryGetComponent<Health>()), Health*>);
static_assert(
        std::is_same_v<decltype(std::declval<const NEcs::EntityRef&>().TryGetComponent<Health>()), const Health*>);
static_assert(
        std::is_same_v<decltype(std::declval<NEcs::EntityRef&>().TryGetComponent<const Health>()), const Health*>);

TEST(EntityRef, PendingReadsNeverExposeQueuedPayloadAndCommittedMethodsUseWorldStorage) {
    NEcs::World world;
    auto entity = world.GetEntityRef(world.QueueCreateEntity())
                          .AddComponent<Health>(100, 100)
                          .AddComponent<std::unique_ptr<int>>(std::make_unique<int>(7));
    EXPECT_TRUE(entity.IsPending());
    EXPECT_FALSE(entity.IsAlive());
    EXPECT_FALSE(entity.HasComponent<Health>());
    EXPECT_EQ(entity.TryGetComponent<Health>(), nullptr);
    EXPECT_EQ(std::as_const(entity).TryGetComponent<Health>(), nullptr);
    NTest::ExpectError(NCommon::EError::NOT_READY, [&] { (void)entity.GetComponent<Health>(); });
    NTest::ExpectError(NCommon::EError::NOT_READY, [&] { (void)std::as_const(entity).GetComponent<Health>(); });
    world.CommitPending();
    EXPECT_FALSE(entity.IsPending());
    EXPECT_TRUE(entity.IsAlive());
    entity.GetComponent<Health>().Damage(20);
    EXPECT_EQ(world.GetComponent<Health>(entity.GetReference().Identity).Current, 80);
    EXPECT_EQ(&entity.GetComponent<Health>(), &world.GetComponent<Health>(entity.GetReference().Identity));
    EXPECT_EQ(std::as_const(entity).GetComponent<Health>().Maximum, 100);
    EXPECT_EQ(*entity.GetComponent<std::unique_ptr<int>>(), 7);
    EXPECT_FALSE(entity.HasComponent<Missing>());
    EXPECT_EQ(entity.TryGetComponent<Missing>(), nullptr);
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { (void)entity.GetComponent<Missing>(); });
}

TEST(EntityRef, StructuralMethodsRemainDeferredAndRejectDuplicateOrImplicitReplace) {
    NEcs::World world;
    auto entity = world.GetEntityRef(world.QueueCreateEntity());
    auto chained = entity.AddComponent<Position>(Position{9});
    EXPECT_EQ(chained.GetReference(), entity.GetReference());
    NTest::ExpectError(NCommon::EError::ALREADY_EXISTS, [&] { entity.AddComponent<Position>(10); });
    world.CommitPending();
    entity.RemoveComponent<Position>();
    EXPECT_EQ(entity.GetComponent<Position>().Value, 9);
    NTest::ExpectError(NCommon::EError::ALREADY_EXISTS, [&] { entity.AddComponent<Position>(11); });
    world.CommitPending();
    EXPECT_FALSE(entity.HasComponent<Position>());
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { entity.RemoveComponent<Position>(); });
    entity.AddComponent<Position>(11);
    EXPECT_FALSE(entity.HasComponent<Position>());
    world.CommitPending();
    entity.Destroy();
    entity.Destroy();
    EXPECT_TRUE(entity.IsAlive());
    EXPECT_EQ(entity.GetComponent<Position>().Value, 11);
    world.CommitPending();
    EXPECT_FALSE(entity.IsAlive());
    EXPECT_FALSE(entity.IsPending());
}

TEST(EntityRef, StaleForeignAndExpiredWorldNeverResolveARecycledEntity) {
    NEcs::EntityRef expired;
    {
        auto world = std::make_unique<NEcs::World>();
        expired = world->GetEntityRef(world->QueueCreateEntity()).AddComponent<Position>(9);
        world->CommitPending();
    }
    EXPECT_FALSE(expired.IsAlive());
    EXPECT_FALSE(expired.IsPending());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)expired.TryGetComponent<Position>(); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { expired.Destroy(); });
    NEcs::EntityRef empty;
    EXPECT_FALSE(empty.IsAlive());
    EXPECT_FALSE(empty.IsPending());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { empty.AddComponent<Position>(); });
    NEcs::World world;
    NEcs::World foreign;
    auto entity = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Position>(3);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)foreign.GetEntityRef(entity.GetReference()); });
    world.CommitPending();
    entity.Destroy();
    world.CommitPending();
    auto replacement = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Position>(17);
    world.CommitPending();
    EXPECT_EQ(replacement.GetReference().Identity.Index, entity.GetReference().Identity.Index);
    EXPECT_NE(replacement.GetReference().Identity.Generation, entity.GetReference().Identity.Generation);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)entity.GetComponent<Position>(); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)entity.HasComponent<Position>(); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { entity.RemoveComponent<Position>(); });
    EXPECT_EQ(replacement.GetComponent<Position>().Value, 17);
}

TEST(EntityRef, FailedPreflightInvalidatesPendingFacade) {
    NEcs::World world;
    auto entity = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Position>(1);
    world.QueueReparent(entity.GetReference(), entity.GetReference());
    EXPECT_THROW(world.CommitPending(), NCommon::Exception);
    EXPECT_FALSE(entity.IsAlive());
    EXPECT_FALSE(entity.IsPending());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)entity.TryGetComponent<Position>(); });
}

TEST(EntityRef, CoarseInvocationChecksDeclarationsAndEscapedFacadeExpires) {
    for (unsigned workers: {0U, 1U, 2U}) {
        NEcs::World world;
        auto host =
                world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Health>(10, 10).AddComponent<Position>(1);
        world.CommitPending();
        NEcs::EntityRef escaped;
        world.RegisterSystem(
                {NEcs::Read<Health>(), NEcs::Write<Position>()},
                [&](NEcs::World& current) {
                    auto entity = current.GetEntityRef(host.GetReference());
                    EXPECT_EQ(std::as_const(entity).GetComponent<Health>().Current, 10);
                    EXPECT_EQ(entity.GetComponent<const Health>().Current, 10);
                    EXPECT_EQ(std::as_const(host).GetComponent<Health>().Current, 10);
                    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)entity.GetComponent<Health>(); });
                    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                                       [&] { (void)host.TryGetComponent<Health>(); });
                    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                                       [&] { (void)entity.HasComponent<Missing>(); });
                    entity.GetComponent<Position>().Value = 7;
                    escaped = entity;
                },
                NEcs::SystemSchedule{.Id = 10});
        if (workers == 0) {
            world.RunSystems();
        } else {
            NCommon::TaskSystem tasks{workers};
            world.RunSystems(tasks);
        }
        EXPECT_EQ(host.GetComponent<Position>().Value, 7);
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)escaped.IsAlive(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE,
                           [&] { (void)std::as_const(escaped).GetComponent<Health>(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { escaped.Destroy(); });
    }
}

TEST(EntityRef, ScopedInvocationHonorsReadWriteEntityBoundsAndMissingDeclaredComponents) {
    NEcs::World world;
    auto first = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Health>(8, 8).AddComponent<Position>(1);
    auto second = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Health>(9, 9).AddComponent<Position>(2);
    world.CommitPending();
    NEcs::EntityRef escaped;
    using NEcs::NInternal::BatchScope;
    using NEcs::NInternal::ScopedBatchContext;
    using NEcs::NInternal::ScopedRead;
    using NEcs::NInternal::ScopedWorldAccess;
    using NEcs::NInternal::ScopedWrite;
    ScopedWorldAccess::Register(
            world,
            {ScopedRead<Health>(BatchScope::Entity(first.GetReference())),
             ScopedWrite<Position>(BatchScope::Entity(first.GetReference())),
             ScopedRead<Missing>(BatchScope::Entity(first.GetReference()))},
            [&](ScopedBatchContext&) {
                auto entity = world.GetEntityRef(first.GetReference());
                EXPECT_EQ(std::as_const(entity).GetComponent<Health>().Current, 8);
                entity.GetComponent<Position>().Value = 14;
                EXPECT_FALSE(entity.HasComponent<Missing>());
                NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)entity.GetComponent<Health>(); });
                NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                                   [&] { (void)std::as_const(second).GetComponent<Health>(); });
                auto outside = world.GetEntityRef(second.GetReference());
                NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)outside.HasComponent<Missing>(); });
                escaped = entity;
            },
            NEcs::SystemSchedule{.Id = 10});
    NCommon::TaskSystem tasks{2};
    world.RunSystems(tasks);
    EXPECT_EQ(first.GetComponent<Position>().Value, 14);
    EXPECT_EQ(second.GetComponent<Position>().Value, 2);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)escaped.TryGetComponent<Position>(); });
}

TEST(EntityRef, InvocationFailureAlsoInvalidatesEscapedCapability) {
    NEcs::World world;
    auto host = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Position>(9);
    world.CommitPending();
    NEcs::EntityRef escaped;
    world.RegisterSystem(
            {NEcs::Read<Position>()},
            [&](NEcs::World& current) {
                escaped = current.GetEntityRef(host.GetReference());
                throw std::runtime_error("test failure");
            },
            NEcs::SystemSchedule{.Id = 10});
    NCommon::TaskSystem tasks{1};
    EXPECT_THROW(world.RunSystems(tasks), std::runtime_error);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)escaped.HasComponent<Position>(); });
    EXPECT_EQ(host.GetComponent<Position>().Value, 9);
}

TEST(EntityRef, DetachedWorkerDoesNotInheritOwnerAccess) {
    NEcs::World world;
    auto host = world.GetEntityRef(world.QueueCreateEntity()).AddComponent<Position>(9);
    world.CommitPending();
    NCommon::TaskSystem tasks{1};
    const auto task = tasks.Submit([&](NCommon::TaskContext&) {
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)host.GetComponent<Position>(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { host.RemoveComponent<Position>(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)world.GetEntityRef(host.GetReference()); });
    });
    tasks.Wait(task);
    EXPECT_EQ(host.GetComponent<Position>().Value, 9);
}
} // namespace
