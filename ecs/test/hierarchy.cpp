#include <atomic>
#include <barrier>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include <ecs/world.h>
#include <gtest/gtest.h>
#include <math/transform.h>
#include <tests/common/test_error.h>

namespace {

using NEcs::EntityReference;
using NEcs::EReparentMode;
using NEcs::World;

EntityReference Create(World& world) {
    return world.GetEntityReference(world.CreateEntity());
}

NMath::Mat4 Translation(float x) {
    NMath::Mat4 matrix = NMath::Mat4::Identity();
    matrix(0, 3) = x;
    return matrix;
}

void ExpectMatrix(const NMath::Mat4& actual, const NMath::Mat4& expected) {
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            EXPECT_NEAR(actual(row, column), expected(row, column), 0.0002F);
        }
    }
}

TEST(Hierarchy, ForestChildrenKeepLocalAndDestroyRecycle) {
    World world;
    const auto parent = Create(world);
    const auto child = Create(world);
    const auto sibling = Create(world);
    const auto grandchild = Create(world);
    world.SetLocalTransform(parent, Translation(5));
    world.SetLocalTransform(child, Translation(2));
    world.DeferReparent(child, parent);
    world.DeferReparent(sibling, parent);
    world.DeferReparent(grandchild, child);
    world.ApplyDeferredStructuralChanges();
    std::uint64_t epoch = 0;
    {
        const auto topology = world.AcquireTopologySnapshot();
        epoch = topology.GetEpoch();
        EXPECT_EQ(topology.GetParent(child), parent);
        const auto children = topology.GetChildren(parent);
        EXPECT_EQ((std::vector<EntityReference>{children.begin(), children.end()}),
                  (std::vector<EntityReference>{child, sibling}));
        EXPECT_TRUE(topology.IsAncestor(parent, grandchild));
        EXPECT_FALSE(topology.IsAncestor(child, child));
        ExpectMatrix(topology.GetLocalTransform(child), Translation(2));
        ExpectMatrix(topology.GetWorldTransform(grandchild), Translation(7));
    }
    world.DeferDestroyEntity(child.Identity);
    world.ApplyDeferredStructuralChanges();
    const auto recycled = Create(world);
    ASSERT_EQ(recycled.Identity.Index, child.Identity.Index);
    EXPECT_NE(recycled.Identity.Generation, child.Identity.Generation);
    const auto topology = world.AcquireTopologySnapshot();
    EXPECT_GT(topology.GetEpoch(), epoch);
    EXPECT_FALSE(topology.IsAlive(child));
    EXPECT_EQ(topology.GetParent(grandchild), EntityReference{});
    EXPECT_EQ(topology.GetParent(recycled), EntityReference{});
    EXPECT_EQ(*topology.GetChildren(parent).begin(), sibling);
    ExpectMatrix(topology.GetLocalTransform(grandchild), NMath::Mat4::Identity());
}

TEST(Hierarchy, KeepWorldSupportsAffineShearRotationScaleAndRootDetach) {
    World world;
    const auto a = Create(world);
    const auto b = Create(world);
    const auto child = Create(world);
    const auto descendant = Create(world);
    NMath::Mat4 parentA = NMath::ComposeTransform({.Translation = {3, 2, -1}, .Scale = {2, 3, 4}});
    parentA(0, 1) = 0.7F;
    NMath::Mat4 parentB = NMath::Mat4::Identity();
    parentB(0, 0) = 0;
    parentB(1, 0) = -2;
    parentB(0, 1) = 3;
    parentB(1, 1) = 0;
    parentB(0, 3) = -10;
    world.SetLocalTransform(a, parentA);
    world.SetLocalTransform(b, parentB);
    world.SetLocalTransform(child, Translation(4));
    world.SetLocalTransform(descendant, Translation(1));
    world.DeferReparent(child, a);
    world.DeferReparent(descendant, child);
    world.ApplyDeferredStructuralChanges();
    const NMath::Mat4 expected = parentA * Translation(4);
    world.DeferReparent(child, b, EReparentMode::KEEP_WORLD);
    world.ApplyDeferredStructuralChanges();
    {
        const auto topology = world.AcquireTopologySnapshot();
        ExpectMatrix(topology.GetWorldTransform(child), expected);
        ExpectMatrix(topology.GetWorldTransform(descendant), expected * Translation(1));
    }
    world.DeferReparent(child, {}, EReparentMode::KEEP_WORLD);
    world.ApplyDeferredStructuralChanges();
    const auto topology = world.AcquireTopologySnapshot();
    ExpectMatrix(topology.GetLocalTransform(child), expected);
    ExpectMatrix(topology.GetWorldTransform(child), expected);
}

TEST(Hierarchy, FailedReparentDoesNotPublishEpochOrPartialLinks) {
    World world;
    World other;
    const auto a = Create(world);
    const auto b = Create(world);
    const auto foreign = Create(other);
    world.DeferReparent(b, a);
    world.ApplyDeferredStructuralChanges();
    const std::uint64_t epoch = world.AcquireTopologySnapshot().GetEpoch();
    for (const auto pair:
         std::vector<std::pair<EntityReference, EntityReference>>{{a, a}, {a, b}, {b, foreign}, {foreign, a}}) {
        world.DeferReparent(pair.first, pair.second);
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { world.ApplyDeferredStructuralChanges(); });
        const auto topology = world.AcquireTopologySnapshot();
        EXPECT_EQ(topology.GetEpoch(), epoch);
        EXPECT_EQ(topology.GetParent(b), a);
        EXPECT_EQ(topology.GetParent(a), EntityReference{});
    }
    NMath::Mat4 singular = NMath::Mat4::Identity();
    singular(1, 1) = 0;
    const auto singularParent = Create(world);
    world.SetLocalTransform(singularParent, singular);
    const std::uint64_t before = world.AcquireTopologySnapshot().GetEpoch();
    world.DeferReparent(b, singularParent, EReparentMode::KEEP_WORLD);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { world.ApplyDeferredStructuralChanges(); });
    const auto topology = world.AcquireTopologySnapshot();
    EXPECT_EQ(topology.GetEpoch(), before);
    EXPECT_EQ(topology.GetParent(b), a);
}

TEST(Hierarchy, StaleReferencesAndInvalidTransformsAreRejected) {
    World world;
    const auto parent = Create(world);
    const auto stale = Create(world);
    world.DestroyEntity(stale.Identity);
    const auto replacement = Create(world);
    world.DeferReparent(stale, parent);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { world.ApplyDeferredStructuralChanges(); });
    {
        const auto topology = world.AcquireTopologySnapshot();
        EXPECT_FALSE(topology.IsAlive(stale));
        EXPECT_EQ(topology.GetParent(replacement), EntityReference{});
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)topology.GetChildren(stale); });
    }
    NMath::Mat4 perspective = NMath::Mat4::Identity();
    perspective(3, 2) = 1;
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { world.SetLocalTransform(parent, perspective); });
}

TEST(Hierarchy, LeaseBlocksCommitBeforeQueueConsumptionAndMayReleaseOnWorker) {
    World world;
    const auto parent = Create(world);
    const auto child = Create(world);
    std::optional<NEcs::TopologySnapshot> topology{world.AcquireTopologySnapshot()};
    const std::uint64_t epoch = topology->GetEpoch();
    world.DeferReparent(child, parent);
    world.DeferDestroyEntity(parent.Identity);
    std::barrier sync{2};
    std::jthread reader{[snapshot = *topology, &sync, child, parent] {
        sync.arrive_and_wait();
        for (int index = 0; index < 1000; ++index) {
            EXPECT_EQ(snapshot.GetParent(child), EntityReference{});
            EXPECT_TRUE(snapshot.IsAlive(parent));
        }
        sync.arrive_and_wait();
    }};
    sync.arrive_and_wait();
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.ApplyDeferredStructuralChanges(); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { (void)world.CreateEntity(); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.AddComponent<int>(child.Identity, 1); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world.SetLocalTransform(child, Translation(1)); });
    EXPECT_EQ(topology->GetEpoch(), epoch);
    topology.reset();
    sync.arrive_and_wait();
    reader.join();
    world.ApplyDeferredStructuralChanges();
    const auto after = world.AcquireTopologySnapshot();
    EXPECT_FALSE(after.IsAlive(parent));
    EXPECT_EQ(after.GetParent(child), EntityReference{});
    EXPECT_EQ(after.GetEpoch(), epoch + 2);
}

TEST(Hierarchy, ChildrenIteratorPinsStorageAndSnapshotOutlivesWorld) {
    std::optional<NEcs::TopologySnapshot> retained;
    EntityReference child;
    {
        auto world = std::make_unique<World>();
        const auto parent = Create(*world);
        child = Create(*world);
        world->DeferReparent(child, parent);
        world->ApplyDeferredStructuralChanges();
        retained = world->AcquireTopologySnapshot();
        auto iterator = retained->GetChildren(parent).begin();
        retained.reset();
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { world->DestroyEntity(child.Identity); });
        EXPECT_EQ(*iterator, child);
        retained = world->AcquireTopologySnapshot();
    }
    EXPECT_TRUE(retained->IsAlive(child));
    World replacement;
    const auto newEntity = Create(replacement);
    EXPECT_NE(retained->GetWorldId(), newEntity.WorldId);
    EXPECT_FALSE(retained->IsAlive(newEntity));
}

TEST(Hierarchy, PlannedBuffersPreserveDeterministicCreateAndReparentOrder) {
    World world;
    world.BeginDeferredStructuralPhase({{.Order = 1, .EntityCreateCount = 1}, {.Order = 2, .EntityCreateCount = 1}});
    auto first = world.CreateDeferredStructuralCommandBuffer(1);
    auto second = world.CreateDeferredStructuralCommandBuffer(2);
    const auto parent = world.GetEntityReference(world.DeferCreateEntity(first));
    const auto child = world.GetEntityReference(world.DeferCreateEntity(second));
    World::DeferReparent(second, child, parent);
    world.SubmitDeferredStructuralCommands(std::move(second));
    world.SubmitDeferredStructuralCommands(std::move(first));
    world.ApplyDeferredStructuralChanges();
    const auto topology = world.AcquireTopologySnapshot();
    EXPECT_EQ(topology.GetParent(child), parent);
}

TEST(Hierarchy, DeepChainAndWideStarUseIterativeTraversal) {
    World world;
    std::vector<EntityReference> nodes;
    for (int index = 0; index < 4096; ++index) {
        nodes.push_back(Create(world));
        world.SetLocalTransform(nodes.back(), Translation(1));
        if (index != 0) {
            world.DeferReparent(nodes.back(), nodes[static_cast<std::size_t>(index - 1)]);
        }
    }
    world.ApplyDeferredStructuralChanges();
    {
        const auto topology = world.AcquireTopologySnapshot();
        EXPECT_TRUE(topology.IsAncestor(nodes.front(), nodes.back()));
        EXPECT_FLOAT_EQ(topology.GetWorldTransform(nodes.back())(0, 3), 4096);
    }
    for (std::size_t index = 2; index < nodes.size(); ++index) {
        world.DeferReparent(nodes[index], nodes.front());
    }
    world.ApplyDeferredStructuralChanges();
    const auto topology = world.AcquireTopologySnapshot();
    std::size_t index = 1;
    for (const auto child: topology.GetChildren(nodes.front())) {
        EXPECT_EQ(child, nodes[index++]);
    }
    EXPECT_EQ(index, nodes.size());
}

TEST(Hierarchy, RandomReparentsMatchIndependentParentForestReference) {
    World world;
    constexpr std::size_t count = 64;
    std::vector<EntityReference> entities;
    std::vector<int> parents(count, -1);
    for (std::size_t index = 0; index < count; ++index) {
        entities.push_back(Create(world));
        world.SetLocalTransform(entities.back(), Translation(static_cast<float>(index + 1)));
    }
    std::mt19937 random{73}; // NOLINT(cert-msc51-cpp): reproducible reference-forest regression.
    for (int step = 0; step < 500; ++step) {
        const std::size_t child = random() % count;
        const int parent = static_cast<int>(random() % (count + 1)) - 1;
        bool cycle = false;
        for (int cursor = parent; cursor != -1; cursor = parents[static_cast<std::size_t>(cursor)]) {
            cycle |= static_cast<std::size_t>(cursor) == child;
        }
        world.DeferReparent(entities[child],
                            parent == -1 ? EntityReference{} : entities[static_cast<std::size_t>(parent)]);
        if (cycle) {
            NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { world.ApplyDeferredStructuralChanges(); });
        } else {
            world.ApplyDeferredStructuralChanges();
            parents[child] = parent;
        }
        const auto topology = world.AcquireTopologySnapshot();
        for (std::size_t index = 0; index < count; ++index) {
            float expected = static_cast<float>(index + 1);
            for (int cursor = parents[index]; cursor != -1; cursor = parents[static_cast<std::size_t>(cursor)]) {
                expected += static_cast<float>(cursor + 1);
            }
            EXPECT_FLOAT_EQ(topology.GetWorldTransform(entities[index])(0, 3), expected);
        }
    }
}

} // namespace
