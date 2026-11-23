#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <vector>

#include <GraphicsEngine/math/transform.h>
#include <GraphicsEngine/scene/scene_manager.h>
#include <GraphicsEngine/scene/spatial_bounds.h>
#include <gtest/gtest.h>

namespace {

using NScene::CaptureSpatialRecord;
using NScene::CaptureSpatialRecords;
using NScene::ESpatialValidity;
using NScene::SpatialAabb;
using NScene::SpatialBoundsComponent;
using NScene::SpatialSphere;
using NScene::TransformSpatialBounds;

SpatialBoundsComponent Box(std::uint64_t revision = 1) {
    return {.LocalBounds = SpatialAabb{{-1, -2, -3}, {1, 2, 3}}, .Mobility = {}, .Revision = revision};
}

TEST(SpatialBounds, AffineRotationNegativeScaleAndShearEncloseEveryCorner) {
    auto matrix = NMath::ComposeTransform(
            {.Translation = {5, -7, 11}, .Rotation = NMath::AngleAxis(0.7F, {0, 0, 1}), .Scale = {-2, 3, 0.5F}});
    matrix(0, 2) += 1.25F;
    const auto bounds = TransformSpatialBounds(Box().LocalBounds, matrix);
    ASSERT_TRUE(bounds);
    for (int mask = 0; mask < 8; ++mask) {
        const std::array<double, 3> point{(mask & 1) != 0 ? 1.0 : -1.0,
                                          (mask & 2) != 0 ? 2.0 : -2.0,
                                          (mask & 4) != 0 ? 3.0 : -3.0};
        std::array<double, 3> transformed{};
        for (std::size_t row = 0; row < 3; ++row) {
            transformed[row] = matrix(row, 3);
            for (std::size_t column = 0; column < 3; ++column) {
                transformed[row] += static_cast<double>(matrix(row, column)) * point[column];
            }
        }
        EXPECT_LE(bounds->Aabb.Min.X, transformed[0]);
        EXPECT_GE(bounds->Aabb.Max.X, transformed[0]);
        EXPECT_LE(bounds->Aabb.Min.Y, transformed[1]);
        EXPECT_GE(bounds->Aabb.Max.Y, transformed[1]);
        EXPECT_LE(bounds->Aabb.Min.Z, transformed[2]);
        EXPECT_GE(bounds->Aabb.Max.Z, transformed[2]);
        EXPECT_LE(std::hypot(transformed[0] - bounds->Sphere.Center.X,
                             transformed[1] - bounds->Sphere.Center.Y,
                             transformed[2] - bounds->Sphere.Center.Z),
                  bounds->Sphere.Radius);
    }
}

TEST(SpatialBounds, IntervalRoundingContainsHighCancellationReferenceCorners) {
    // Fixed seed makes the numerical regression corpus reproducible.
    // NOLINTNEXTLINE(cert-msc51-cpp)
    std::mt19937 random{115};
    std::uniform_real_distribution<double> coordinate{-1e4, 1e4};
    for (int iteration = 0; iteration < 128; ++iteration) {
        const SpatialAabb box{{coordinate(random), coordinate(random), coordinate(random)}, {1e4, 1e4, 1e4}};
        auto matrix = NMath::Mat4::Identity();
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                matrix(row, column) = static_cast<float>(coordinate(random));
            }
        }
        const auto bounds = TransformSpatialBounds(box, matrix);
        ASSERT_TRUE(bounds);
        for (int mask = 0; mask < 8; ++mask) {
            const std::array<long double, 3> point{(mask & 1) != 0 ? box.Max.X : box.Min.X,
                                                   (mask & 2) != 0 ? box.Max.Y : box.Min.Y,
                                                   (mask & 4) != 0 ? box.Max.Z : box.Min.Z};
            const std::array<double, 3> min{bounds->Aabb.Min.X, bounds->Aabb.Min.Y, bounds->Aabb.Min.Z};
            const std::array<double, 3> max{bounds->Aabb.Max.X, bounds->Aabb.Max.Y, bounds->Aabb.Max.Z};
            for (std::size_t row = 0; row < 3; ++row) {
                long double reference = matrix(row, 3);
                for (std::size_t column = 0; column < 3; ++column) {
                    reference += static_cast<long double>(matrix(row, column)) * point[column];
                }
                EXPECT_LE(static_cast<long double>(min[row]), reference);
                EXPECT_GE(static_cast<long double>(max[row]), reference);
            }
        }
    }
}

TEST(SpatialBounds, SphereAdapterDegenerateBoundsAndGlobalCoordinates) {
    const SpatialSphere sphere{{10, 20, 30}, 2};
    const auto matrix = NMath::ComposeTransform({.Translation = {1000, 0, 0}, .Scale = {-2, 3, 4}});
    const auto bounds = TransformSpatialBounds(sphere, matrix);
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->Aabb.Min.X, 976, 1e-10);
    EXPECT_NEAR(bounds->Aabb.Max.X, 984, 1e-10);
    EXPECT_NEAR(bounds->Aabb.Min.Y, 54, 1e-10);
    EXPECT_NEAR(bounds->Aabb.Max.Z, 128, 1e-10);
    EXPECT_TRUE(TransformSpatialBounds(SpatialSphere{{}, 0}, NMath::Mat4::Identity()));
    EXPECT_TRUE(TransformSpatialBounds(SpatialAabb{{2, 2, 2}, {2, 2, 2}}, NMath::Mat4::Identity()));
    // Camera/render origin is deliberately absent from this API; global coordinates never rebase.
    const auto repeat = TransformSpatialBounds(sphere, matrix);
    EXPECT_EQ(bounds, repeat);
}

TEST(SpatialBounds, RejectsNonfiniteInvertedExtremeAndProjectiveInputs) {
    const auto identity = NMath::Mat4::Identity();
    for (double bad: {std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::infinity(),
                      -std::numeric_limits<double>::infinity(),
                      1e100}) {
        EXPECT_FALSE(TransformSpatialBounds(SpatialSphere{{bad, 0, 0}, 1}, identity));
        EXPECT_FALSE(TransformSpatialBounds(SpatialSphere{{}, bad}, identity));
        EXPECT_FALSE(TransformSpatialBounds(SpatialAabb{{bad, 0, 0}, {bad, 1, 1}}, identity));
    }
    EXPECT_FALSE(TransformSpatialBounds(SpatialSphere{{}, -1}, identity));
    EXPECT_FALSE(TransformSpatialBounds(SpatialAabb{{1, 0, 0}, {-1, 0, 0}}, identity));
    auto matrix = identity;
    matrix(3, 0) = 0.01F;
    EXPECT_FALSE(TransformSpatialBounds(Box().LocalBounds, matrix));
    matrix = identity;
    matrix(0, 0) = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(TransformSpatialBounds(Box().LocalBounds, matrix));
    matrix = identity;
    matrix(0, 0) = 1e30F;
    EXPECT_FALSE(TransformSpatialBounds(Box().LocalBounds, matrix));
}

class SpatialRecords: public testing::Test {
protected:
    NScene::SceneManager Scenes;
    NScene::SceneHandle Scene = Scenes.CreateScene("spatial");
    std::shared_ptr<NScene::Scene> Lease = Scenes.Acquire(Scene);

    NEcs::EntityRef Add(SpatialBoundsComponent source = Box()) {
        return Lease->CreateEntity().AddComponent<SpatialBoundsComponent>(source);
    }
};

TEST_F(SpatialRecords, PublishesOnlyCommittedComponentsAndRetainsOldValues) {
    auto entity = Add();
    const NScene::SpatialIdentity identity{Scene, entity.GetReference()};
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::STALE_ENTITY);
    EXPECT_TRUE(CaptureSpatialRecords(Scenes, Scene).empty());
    Lease->CommitPending();
    const auto first = CaptureSpatialRecord(Scenes, identity);
    ASSERT_EQ(first.Validity, ESpatialValidity::VALID);
    EXPECT_EQ(first.SourceRevision, 1U);
    entity.RemoveComponent<SpatialBoundsComponent>();
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).SourceRevision, 1U);
    Lease->CommitPending();
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::MISSING_BOUNDS);
    entity.AddComponent<SpatialBoundsComponent>(Box(2));
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::MISSING_BOUNDS);
    Lease->CommitPending();
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).SourceRevision, 2U);
    EXPECT_EQ(first.SourceRevision, 1U);
    entity.Destroy();
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::VALID);
    Lease->CommitPending();
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::STALE_ENTITY);
    EXPECT_TRUE(first.Bounds);
}

TEST_F(SpatialRecords, RejectsStaleForeignAndUnloadedIdentitiesWithoutRetainingScene) {
    auto entity = Add();
    Lease->CommitPending();
    const NScene::SpatialIdentity identity{Scene, entity.GetReference()};
    const auto snapshot = CaptureSpatialRecords(Scenes, Scene);
    ASSERT_EQ(snapshot.size(), 1U);
    const auto foreign = Scenes.CreateScene();
    EXPECT_EQ(CaptureSpatialRecord(Scenes, {foreign, identity.Entity}).Validity, ESpatialValidity::FOREIGN_ENTITY);
    entity.Destroy();
    Lease->CommitPending();
    auto recycled = Add();
    Lease->CommitPending();
    EXPECT_EQ(recycled.GetReference().Identity.Index, identity.Entity.Identity.Index);
    EXPECT_NE(recycled.GetReference().Identity.Generation, identity.Entity.Identity.Generation);
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::STALE_ENTITY);
    const std::weak_ptr<NScene::Scene> weak = Lease;
    Scenes.Unload(Scene);
    Lease.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::SCENE_UNAVAILABLE);
    const auto nextScene = Scenes.CreateScene();
    EXPECT_NE(Scene, nextScene);
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::SCENE_UNAVAILABLE);
    EXPECT_EQ(snapshot[0].Validity, ESpatialValidity::VALID);
}

TEST_F(SpatialRecords, DisabledEntityAndBoundsRespectCommitBoundary) {
    auto entity = Add();
    Lease->CommitPending();
    const NScene::SpatialIdentity identity{Scene, entity.GetReference()};
    auto& world = Lease->GetWorld();
    world.QueueSetEnabled(identity.Entity, false);
    EXPECT_EQ(CaptureSpatialRecords(Scenes, Scene).size(), 1U);
    Lease->CommitPending();
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::DISABLED);
    EXPECT_TRUE(CaptureSpatialRecords(Scenes, Scene).empty());
    world.QueueSetEnabled(identity.Entity, true);
    world.QueueSetComponentEnabled<SpatialBoundsComponent>(identity.Entity, false);
    Lease->CommitPending();
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::DISABLED);
    EXPECT_TRUE(CaptureSpatialRecords(Scenes, Scene).empty());
    world.QueueSetComponentEnabled<SpatialBoundsComponent>(identity.Entity, true);
    Lease->CommitPending();
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::VALID);
}

TEST_F(SpatialRecords, ProviderAndClassificationDiagnostics) {
    auto entity = Lease->CreateEntity();
    Lease->CommitPending();
    const NScene::SpatialIdentity identity{Scene, entity.GetReference()};
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).Validity, ESpatialValidity::MISSING_BOUNDS);
    for (auto mobility: {std::optional<NScene::ESpatialMobility>{},
                         std::optional{NScene::ESpatialMobility::STATIC},
                         std::optional{NScene::ESpatialMobility::DYNAMIC}}) {
        auto source = Box(42);
        source.Mobility = mobility;
        const auto record = CaptureSpatialRecord(Scenes, identity, source);
        EXPECT_EQ(record.Validity, ESpatialValidity::VALID);
        EXPECT_EQ(record.SourceRevision, 42U);
        EXPECT_EQ(record.Mobility, mobility);
    }
    auto source = Box(0);
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity, source).Validity, ESpatialValidity::INVALID_BOUNDS);
    source = Box();
    source.Mobility = static_cast<NScene::ESpatialMobility>(99);
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity, source).Validity, ESpatialValidity::INVALID_BOUNDS);
    source = Box();
    source.LocalBounds = SpatialSphere{{}, -1};
    const auto invalid = CaptureSpatialRecord(Scenes, identity, source);
    EXPECT_EQ(invalid.Validity, ESpatialValidity::INVALID_BOUNDS);
    EXPECT_FALSE(invalid.Bounds);
}

TEST_F(SpatialRecords, AncestorLocalChangesAndReparentingInvalidateExactRevisionChain) {
    auto parent = Lease->CreateEntity();
    auto other = Lease->CreateEntity();
    auto child = Add();
    Lease->CommitPending();
    auto& world = Lease->GetWorld();
    world.QueueReparent(child.GetReference(), parent.GetReference());
    Lease->CommitPending();
    const NScene::SpatialIdentity identity{Scene, child.GetReference()};
    const auto first = CaptureSpatialRecord(Scenes, identity);
    ASSERT_EQ(first.TransformRevisions.size(), 2U);
    world.SetLocalTransform(parent.GetReference(), NMath::ComposeTransform({.Translation = {10, 0, 0}}));
    Lease->CommitPending();
    const auto moved = CaptureSpatialRecord(Scenes, identity);
    EXPECT_NE(moved.TransformRevisions, first.TransformRevisions);
    EXPECT_NEAR(moved.Bounds->Aabb.Min.X, first.Bounds->Aabb.Min.X + 10, 1e-10);
    EXPECT_EQ(moved.SourceRevision, first.SourceRevision);
    world.QueueReparent(child.GetReference(), other.GetReference());
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).TransformRevisions, moved.TransformRevisions);
    Lease->CommitPending();
    const auto reparented = CaptureSpatialRecord(Scenes, identity);
    EXPECT_NE(reparented.TransformRevisions, moved.TransformRevisions);
    EXPECT_EQ(reparented.TransformRevisions[1].Entity, other.GetReference());
    EXPECT_EQ(CaptureSpatialRecord(Scenes, identity).TransformRevisions, reparented.TransformRevisions);
}

TEST_F(SpatialRecords, SnapshotOrderIsDeterministicAndIndependentOfActiveScene) {
    for (int index = 0; index < 16; ++index) {
        (void)Add();
    }
    Lease->CommitPending();
    const auto one = CaptureSpatialRecords(Scenes, Scene);
    Scenes.Activate(Scenes.CreateScene());
    const auto two = CaptureSpatialRecords(Scenes, Scene);
    ASSERT_EQ(one.size(), 16U);
    ASSERT_EQ(two.size(), one.size());
    for (std::size_t index = 0; index < one.size(); ++index) {
        EXPECT_EQ(one[index].Identity, two[index].Identity);
        EXPECT_EQ(one[index].Bounds, two[index].Bounds);
        if (index != 0) {
            EXPECT_LT(one[index - 1].Identity.Entity.Identity.Index, one[index].Identity.Entity.Identity.Index);
        }
    }
}

} // namespace
