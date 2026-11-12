#include <array>
#include <future>
#include <limits>
#include <optional>
#include <semaphore>
#include <vector>

#include <GraphicsEngine/ecs/world.h>
#include <ecs/internal/adaptive_traversal.h>
#include <ecs/internal/transform_resolver.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {
using NEcs::EntityReference;
using NEcs::EReparentMode;
using NEcs::World;
using NEcs::NInternal::ResolvedTransforms;
using NEcs::NInternal::TransformResolver;

EntityReference Create(World& world) {
    return world.GetEntityReference(world.CreateEntity());
}
NMath::Mat4 Translation(float x) {
    auto result = NMath::Mat4::Identity();
    result(0, 3) = x;
    return result;
}
void ExpectMatrix(const NMath::Mat4& actual, const NMath::Mat4& expected) {
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            EXPECT_FLOAT_EQ(actual(row, column), expected(row, column));
        }
    }
}
ResolvedTransforms Resolve(TransformResolver& resolver,
                           World& world,
                           NCommon::TaskSystem& tasks,
                           std::span<const EntityReference> required) {
    const auto operation = resolver.ScheduleResolve(world.AcquireTopologySnapshot(), required, tasks);
    tasks.Wait(operation.GetCompletion());
    EXPECT_EQ(tasks.GetStatus(operation.GetCompletion()), NCommon::ETaskStatus::COMPLETED);
    return operation.ReadResolved(tasks);
}

TEST(TransformResolver, ParentChildAndIndependentEditsPreserveImmutableResults) {
    for (const unsigned workers: {1U, 4U}) {
        World world;
        NCommon::TaskSystem tasks{workers};
        TransformResolver resolver;
        const auto parent = Create(world);
        const auto child = Create(world);
        const auto unrelated = Create(world);
        world.DeferReparent(child, parent);
        world.ApplyDeferredStructuralChanges();
        const std::array required{child};
        const auto first = Resolve(resolver, world, tasks, required);
        EXPECT_EQ(first.GetResolvedCount(), 2U);
        EXPECT_EQ(first.GetRecomputedCount(), 2U);
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)first.Get(unrelated); });
        const auto hit = Resolve(resolver, world, tasks, required);
        EXPECT_EQ(hit.GetRecomputedCount(), 0U);
        EXPECT_EQ(hit.Get(child).WorldRevision, first.Get(child).WorldRevision);
        world.SetLocalTransform(parent, Translation(5));
        const auto parentOnly = Resolve(resolver, world, tasks, required);
        EXPECT_EQ(parentOnly.GetRecomputedCount(), 2U);
        ExpectMatrix(parentOnly.Get(child).Value, Translation(5));
        world.SetLocalTransform(child, Translation(2));
        const auto childOnly = Resolve(resolver, world, tasks, required);
        EXPECT_EQ(childOnly.GetRecomputedCount(), 1U);
        EXPECT_FALSE(childOnly.Get(parent).Recomputed);
        ExpectMatrix(childOnly.Get(child).Value, Translation(7));
        world.SetLocalTransform(parent, Translation(10));
        world.SetLocalTransform(child, Translation(3));
        const auto both = Resolve(resolver, world, tasks, required);
        EXPECT_EQ(both.GetRecomputedCount(), 2U);
        ExpectMatrix(both.Get(child).Value, Translation(13));
        ExpectMatrix(first.Get(child).Value, NMath::Mat4::Identity());
        EXPECT_EQ(first.Get(child).AppliedLocalRevision, first.Get(parent).AppliedLocalRevision);
        // Writing a parent does not invalidate or revise its descendants eagerly.
        EXPECT_EQ(both.Get(child).AppliedLocalRevision.Value, 2U);
    }
}

TEST(TransformResolver, RequiredVisibleSubsetAndMultipleViewsDoNotVisitUnrelatedSubtrees) {
    for (const unsigned workers: {1U, 4U}) {
        World world;
        NCommon::TaskSystem tasks{workers};
        TransformResolver resolver;
        const auto root = Create(world);
        std::vector<EntityReference> children;
        for (std::size_t index = 0; index < 1024; ++index) {
            children.push_back(Create(world));
            world.DeferReparent(children.back(), root);
        }
        world.ApplyDeferredStructuralChanges();
        const std::array firstView{children.front(), children.front()};
        EXPECT_EQ(Resolve(resolver, world, tasks, firstView).GetRecomputedCount(), 2U);
        world.SetLocalTransform(root, Translation(8));
        const std::array secondView{children.back()};
        EXPECT_EQ(Resolve(resolver, world, tasks, secondView).GetRecomputedCount(), 2U);
        // Child omitted from the preceding view still detects its changed parent later.
        const auto oldView = Resolve(resolver, world, tasks, firstView);
        EXPECT_EQ(oldView.GetResolvedCount(), 2U);
        EXPECT_EQ(oldView.GetRecomputedCount(), 1U);
        ExpectMatrix(oldView.Get(children.front()).Value, Translation(8));
        const auto all = Resolve(resolver, world, tasks, children);
        EXPECT_EQ(all.GetResolvedCount(), 1025U);
        EXPECT_EQ(all.GetRecomputedCount(), 1022U);
        for (const auto child: children) {
            ExpectMatrix(all.Get(child).Value, Translation(8));
        }
    }
}

TEST(TransformResolver, MixedParallelBranchesMatchSerialHierarchyReference) {
    for (const unsigned workers: {1U, 4U}) {
        World world;
        NCommon::TaskSystem tasks{workers};
        TransformResolver resolver;
        std::vector<EntityReference> entities;
        for (std::size_t index = 0; index < 600; ++index) {
            entities.push_back(Create(world));
            if (index != 0) {
                world.DeferReparent(entities.back(), entities[(index - 1) / 3]);
            }
        }
        world.ApplyDeferredStructuralChanges();
        (void)Resolve(resolver, world, tasks, entities);
        for (std::size_t round = 0; round < 5; ++round) {
            for (std::size_t index = round; index < entities.size(); index += 7) {
                world.SetLocalTransform(entities[index], Translation(static_cast<float>(index + round)));
            }
            const auto result = Resolve(resolver, world, tasks, entities);
            const auto topology = world.AcquireTopologySnapshot();
            for (const auto entity: entities) {
                ExpectMatrix(result.Get(entity).Value, topology.GetWorldTransform(entity));
            }
        }
    }
}

TEST(TransformResolver, DeepBambooIsIterativeAndCacheHitsDoNotMultiply) {
    World world;
    NCommon::TaskSystem tasks{4};
    TransformResolver resolver;
    std::vector<EntityReference> chain;
    chain.reserve(10000);
    for (std::size_t index = 0; index < 10000; ++index) {
        chain.push_back(Create(world));
    }
    for (std::size_t index = chain.size(); index > 1; --index) {
        world.DeferReparent(chain[index - 1], chain[index - 2]);
    }
    world.ApplyDeferredStructuralChanges();
    const std::array required{chain.back()};
    EXPECT_EQ(Resolve(resolver, world, tasks, required).GetResolvedCount(), chain.size());
    EXPECT_EQ(Resolve(resolver, world, tasks, required).GetRecomputedCount(), 0U);
    world.SetLocalTransform(chain.front(), Translation(1));
    const auto result = Resolve(resolver, world, tasks, required);
    EXPECT_EQ(result.GetRecomputedCount(), chain.size());
    ExpectMatrix(result.Get(chain.back()).Value, Translation(1));
}

TEST(TransformResolver, ReparentModesAndDestroyedParentInvalidateOnlyAffectedEdges) {
    World world;
    NCommon::TaskSystem tasks{4};
    TransformResolver resolver;
    const auto left = Create(world);
    const auto right = Create(world);
    const auto child = Create(world);
    world.SetLocalTransform(left, Translation(5));
    world.SetLocalTransform(right, Translation(9));
    world.SetLocalTransform(child, Translation(2));
    world.DeferReparent(child, left);
    world.ApplyDeferredStructuralChanges();
    const std::array required{left, right, child};
    const auto first = Resolve(resolver, world, tasks, required);
    world.DeferReparent(child, right, EReparentMode::KEEP_LOCAL);
    world.ApplyDeferredStructuralChanges();
    const auto moved = Resolve(resolver, world, tasks, required);
    EXPECT_EQ(moved.GetRecomputedCount(), 1U);
    EXPECT_EQ(moved.Get(child).AppliedLocalRevision, first.Get(child).AppliedLocalRevision);
    ExpectMatrix(moved.Get(child).Value, Translation(11));
    world.DeferReparent(child, left, EReparentMode::KEEP_WORLD);
    world.ApplyDeferredStructuralChanges();
    const auto kept = Resolve(resolver, world, tasks, required);
    EXPECT_EQ(kept.GetRecomputedCount(), 1U);
    ExpectMatrix(kept.Get(child).Value, moved.Get(child).Value);
    world.DeferDestroyEntity(left.Identity);
    world.ApplyDeferredStructuralChanges();
    const std::array surviving{child, right};
    const auto detached = Resolve(resolver, world, tasks, surviving);
    EXPECT_EQ(detached.GetRecomputedCount(), 1U);
    ExpectMatrix(detached.Get(child).Value, Translation(6));
}

TEST(TransformResolver, FailureDoesNotPublishEvenSuccessfullyComputedAncestors) {
    World world;
    NCommon::TaskSystem tasks{4};
    TransformResolver resolver;
    const auto parent = Create(world);
    const auto child = Create(world);
    world.DeferReparent(child, parent);
    world.ApplyDeferredStructuralChanges();
    const std::array required{child};
    const auto first = Resolve(resolver, world, tasks, required);
    auto huge = NMath::Mat4::Identity();
    huge(0, 0) = std::numeric_limits<float>::max();
    world.SetLocalTransform(parent, huge);
    world.SetLocalTransform(child, huge);
    const auto failed = resolver.ScheduleResolve(world.AcquireTopologySnapshot(), required, tasks);
    tasks.Wait(failed.GetCompletion());
    EXPECT_EQ(tasks.GetGroupStatus(failed.GetTraversal()), NCommon::ETaskGroupStatus::FAILED);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)failed.ReadResolved(tasks); });
    ExpectMatrix(first.Get(parent).Value, NMath::Mat4::Identity());
    world.SetLocalTransform(child, NMath::Mat4::Identity());
    const auto retry = Resolve(resolver, world, tasks, required);
    EXPECT_EQ(retry.GetRecomputedCount(), 2U);
    EXPECT_EQ(retry.Get(parent).WorldRevision.Value, first.Get(parent).WorldRevision.Value + 1);
    ExpectMatrix(retry.Get(child).Value, huge);
}

TEST(TransformResolver, StaleForeignNullAndRecycledEntitiesAreRejectedWithoutPoisoningResolver) {
    World world;
    World foreign;
    NCommon::TaskSystem tasks{1};
    TransformResolver resolver;
    const auto stale = Create(world);
    const std::array required{stale};
    const auto old = Resolve(resolver, world, tasks, required);
    world.DeferDestroyEntity(stale.Identity);
    world.ApplyDeferredStructuralChanges();
    const auto fresh = Create(world);
    ASSERT_EQ(fresh.Identity.Index, stale.Identity.Index);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { (void)resolver.ScheduleResolve(world.AcquireTopologySnapshot(), required, tasks); });
    const std::array null{EntityReference{}};
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { (void)resolver.ScheduleResolve(world.AcquireTopologySnapshot(), null, tasks); });
    const std::array crossWorld{Create(foreign)};
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { (void)resolver.ScheduleResolve(world.AcquireTopologySnapshot(), crossWorld, tasks); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE,
                       [&] { (void)resolver.ScheduleResolve(foreign.AcquireTopologySnapshot(), crossWorld, tasks); });
    const std::array live{fresh};
    const auto result = Resolve(resolver, world, tasks, live);
    EXPECT_EQ(result.GetRecomputedCount(), 1U);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)result.Get(stale); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)old.Get(fresh); });
    ExpectMatrix(old.Get(stale).Value, NMath::Mat4::Identity());
}

TEST(TransformResolver, CancellationRejectsPartialReadsPinsLocalsAndReleasesAdmission) {
    World world;
    NCommon::TaskSystem tasks{1};
    TransformResolver resolver;
    const std::array required{Create(world)};
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    const auto blocker = tasks.Submit([&](NCommon::TaskContext&) {
        entered.release();
        release.acquire();
    });
    entered.acquire();
    const auto cancelled = resolver.ScheduleResolve(world.AcquireTopologySnapshot(), required, tasks);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)cancelled.ReadResolved(tasks); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE,
                       [&] { (void)world.SetLocalTransform(required.front(), Translation(2)); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE,
                       [&] { (void)resolver.ScheduleResolve(world.AcquireTopologySnapshot(), required, tasks); });
    tasks.CancelGroup(cancelled.GetTraversal());
    release.release();
    tasks.Wait(blocker);
    tasks.WaitGroup(cancelled.GetTraversal());
    tasks.Wait(cancelled.GetCompletion());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)cancelled.ReadResolved(tasks); });
    world.SetLocalTransform(required.front(), Translation(2));
    const auto retry = Resolve(resolver, world, tasks, required);
    EXPECT_EQ(retry.GetRecomputedCount(), 1U);
    ExpectMatrix(retry.Get(required.front()).Value, Translation(2));
}

TEST(TransformResolver, EmptyClosureAndResultsOutliveResolverAndWorld) {
    NCommon::TaskSystem tasks{2};
    std::optional<ResolvedTransforms> result;
    EntityReference entity;
    {
        World world;
        TransformResolver resolver;
        EXPECT_EQ(Resolve(resolver, world, tasks, {}).GetResolvedCount(), 0U);
        entity = Create(world);
        world.SetLocalTransform(entity, Translation(4));
        const std::array required{entity};
        result.emplace(Resolve(resolver, world, tasks, required));
    }
    ExpectMatrix(result->Get(entity).Value, Translation(4));
}

TEST(TransformResolver, RevisionRolloverNeverAliasesPreviousEra) {
    using NEcs::NInternal::NextTransformRevision;
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(NextTransformRevision(NCommon::VersionStamp{1, limit}), (NCommon::VersionStamp{2, 0}));
    NTest::ExpectError(NCommon::EError::INVALID_STATE,
                       [&] { (void)NextTransformRevision(NCommon::VersionStamp{limit, limit}); });
}
TEST(TransformResolver, PreviouslyInvisibleCandidateUsesCurrentBoundsAfterParentMove) {
    World world;
    NCommon::TaskSystem tasks{2};
    TransformResolver resolver;
    const auto parent = Create(world);
    const auto candidate = Create(world);
    world.SetLocalTransform(parent, Translation(100));
    world.DeferReparent(candidate, parent);
    world.ApplyDeferredStructuralChanges();
    const std::array required{candidate};
    const auto previous = Resolve(resolver, world, tasks, required);
    const auto intersectsView = [](const ResolvedTransforms& transforms, EntityReference entity) {
        // Unit local bounds projected onto the view's x interval [-2, 2].
        const auto center = transforms.Get(entity).Value * NMath::Vec4{0, 0, 0, 1};
        return center.X - 1 <= 2 && center.X + 1 >= -2;
    };
    EXPECT_FALSE(intersectsView(previous, candidate));
    world.SetLocalTransform(parent, Translation(0));
    // All candidates without conservative bounds are required, including those
    // invisible in the previous view; visibility never selects the resolve closure.
    const auto current = Resolve(resolver, world, tasks, required);
    EXPECT_TRUE(intersectsView(current, candidate));
    EXPECT_FALSE(intersectsView(previous, candidate));
}

TEST(TransformResolver, CancellingPublicationDoesNotReleaseRunningTraversalOrPublishCache) {
    World world;
    NCommon::TaskSystem tasks{1};
    TransformResolver resolver;
    const std::array required{Create(world)};
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    const auto blocker = tasks.Submit([&](NCommon::TaskContext&) {
        entered.release();
        release.acquire();
    });
    entered.acquire();
    const auto operation = resolver.ScheduleResolve(world.AcquireTopologySnapshot(), required, tasks);
    tasks.Cancel(operation.GetCompletion());
    NTest::ExpectError(NCommon::EError::INVALID_STATE,
                       [&] { world.SetLocalTransform(required.front(), Translation(1)); });
    release.release();
    tasks.Wait(blocker);
    tasks.WaitGroup(operation.GetTraversal());
    tasks.Wait(operation.GetCompletion());
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)operation.ReadResolved(tasks); });
    const auto retry = Resolve(resolver, world, tasks, required);
    EXPECT_EQ(retry.GetRecomputedCount(), 1U);
}

TEST(TransformResolver, SubmittedResolveOwnsStateAfterFacadeDestruction) {
    World world;
    NCommon::TaskSystem tasks{1};
    const std::array required{Create(world)};
    NEcs::NInternal::TransformResolve operation;
    {
        TransformResolver resolver;
        operation = resolver.ScheduleResolve(world.AcquireTopologySnapshot(), required, tasks);
    }
    tasks.Wait(operation.GetCompletion());
    EXPECT_EQ(operation.ReadResolved(tasks).GetResolvedCount(), 1U);
}

} // namespace
