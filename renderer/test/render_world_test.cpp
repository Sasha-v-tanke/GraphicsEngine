#include <optional>

#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/renderer/render_components.h>
#include <GraphicsEngine/renderer/render_world.h>
#include <GraphicsEngine/resources/resource_identity.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <engine/controller/frame_scheduler.h>
#include <gtest/gtest.h>
#include <renderer/extraction.h>
#include <tests/common/test_error.h>

namespace NResources {

class Material final {};

class MeshData final {};

} // namespace NResources

namespace {

using NEngine::EngineConfig;
using NEngine::NController::FrameHandle;
using NEngine::NController::FrameScheduler;
using NRenderer::CameraComponent;
using NRenderer::RenderableComponent;
using NRenderer::RenderWorld;
using NRenderer::TransformComponent;
using NResources::Material;
using NResources::MeshData;
using NResources::ResourceIdentity;
using NResources::ResourceManager;

FrameHandle BeginExtractionFrame(FrameScheduler& scheduler) {
    const std::optional<FrameHandle> frame = scheduler.TryAcquireFrame();

    EXPECT_TRUE(frame.has_value());

    if (!frame.has_value()) {
        return {};
    }

    scheduler.ArmFrame(*frame);
    scheduler.BeginUpdate(*frame);
    scheduler.EndUpdate(*frame);
    scheduler.SignalDraw(*frame);
    scheduler.BeginFinalize(*frame);

    return *frame;
}

TEST(RenderWorld, RequiresDrawCheckpointBeforeExtraction) {
    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};
    NEcs::World world;

    const FrameHandle frame = *scheduler.TryAcquireFrame();
    scheduler.ArmFrame(frame);
    scheduler.BeginUpdate(frame);
    scheduler.EndUpdate(frame);

    const NEngine::NController::FrameStorage storage = scheduler.GetFrameStorage(frame);

    NTest::ExpectError(NCommon::EError::INVALID_STATE,
                       [&] { static_cast<void>(NRenderer::NInternal::ExtractRenderWorld(world, storage)); });

    scheduler.SignalDraw(frame);

    NTest::ExpectError(NCommon::EError::INVALID_STATE,
                       [&] { static_cast<void>(NRenderer::NInternal::ExtractRenderWorld(world, storage)); });

    scheduler.BeginFinalize(frame);

    const RenderWorld& renderWorld = NRenderer::NInternal::ExtractRenderWorld(world, storage);

    EXPECT_TRUE(renderWorld.GetViews().empty());
    EXPECT_TRUE(renderWorld.GetObjects().empty());
}

TEST(RenderWorld, IsolatesWorldAndResourceMutationsAfterExtraction) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> firstMesh =
            resources.Request<MeshData>(ResourceIdentity{"mesh", "first"});
    const NResources::ResourceHandle<MeshData> secondMesh =
            resources.Request<MeshData>(ResourceIdentity{"mesh", "second"});
    const NResources::ResourceHandle<Material> material =
            resources.Request<Material>(ResourceIdentity{"material", "first"});

    NEcs::World world;

    const NEcs::Entity camera = world.CreateEntity();
    world.AddComponent<TransformComponent>(camera,
                                           TransformComponent{
                                                   .Value =
                                                           NMath::Transform{
                                                                   .Translation = {.X = 0.0F, .Y = 2.0F, .Z = 5.0F},
                                                           },
                                           });
    world.AddComponent<CameraComponent>(camera,
                                        CameraComponent{
                                                .Projection =
                                                        NMath::PerspectiveProjection{
                                                                .VerticalFovRadians = 1.0F,
                                                                .AspectRatio = 1.5F,
                                                                .NearPlane = 0.1F,
                                                                .FarPlane = 500.0F,
                                                        },
                                        });

    const NEcs::Entity object = world.CreateEntity();
    world.AddComponent<TransformComponent>(object,
                                           TransformComponent{
                                                   .Value =
                                                           NMath::Transform{
                                                                   .Translation = {.X = 1.0F, .Y = 2.0F, .Z = 3.0F},
                                                           },
                                           });
    world.AddComponent<RenderableComponent>(object,
                                            RenderableComponent{
                                                    .Mesh = firstMesh,
                                                    .Material = material,
                                            });

    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};
    const FrameHandle frame = BeginExtractionFrame(scheduler);
    const NEngine::NController::FrameStorage storage = scheduler.GetFrameStorage(frame);
    const RenderWorld& renderWorld = NRenderer::NInternal::ExtractRenderWorld(world, storage);

    ASSERT_EQ(renderWorld.GetViews().size(), 1);
    ASSERT_EQ(renderWorld.GetObjects().size(), 1);

    world.GetComponent<TransformComponent>(camera).Value.Translation.Y = 99.0F;
    world.GetComponent<CameraComponent>(camera).Projection.FarPlane = 42.0F;
    world.GetComponent<TransformComponent>(object).Value.Translation.X = 88.0F;
    world.GetComponent<RenderableComponent>(object).Mesh = secondMesh;
    world.DestroyEntity(object);

    const NRenderer::RenderView& view = renderWorld.GetViews().front();
    const NRenderer::RenderObject& renderObject = renderWorld.GetObjects().front();

    EXPECT_EQ(view.Id.Index, camera.Index);
    EXPECT_EQ(view.Id.Generation, camera.Generation);
    EXPECT_FLOAT_EQ(view.WorldTransform.Translation.Y, 2.0F);
    EXPECT_FLOAT_EQ(view.Projection.FarPlane, 500.0F);

    EXPECT_EQ(renderObject.Id.Index, object.Index);
    EXPECT_EQ(renderObject.Id.Generation, object.Generation);
    EXPECT_FLOAT_EQ(renderObject.WorldTransform.Translation.X, 1.0F);
    EXPECT_EQ(renderObject.Mesh, firstMesh);
    EXPECT_EQ(renderObject.Material, material);
}

TEST(RenderWorld, BindsSnapshotLifetimeToFrameSlotGeneration) {
    NEcs::World world;

    const NEcs::Entity object = world.CreateEntity();
    world.AddComponent<TransformComponent>(object);
    world.AddComponent<RenderableComponent>(object);

    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle firstFrame = BeginExtractionFrame(scheduler);
    const NEngine::NController::FrameStorage firstStorage = scheduler.GetFrameStorage(firstFrame);
    const RenderWorld& firstWorld = NRenderer::NInternal::ExtractRenderWorld(world, firstStorage);
    const NRenderer::RenderFrameIdentity firstIdentity = firstWorld.GetFrame();

    ASSERT_EQ(firstWorld.GetObjects().size(), 1);

    scheduler.CompleteFrame(firstFrame);
    scheduler.RecycleFrame(firstFrame);

    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(firstStorage.GetState()); });

    const FrameHandle secondFrame = BeginExtractionFrame(scheduler);
    const NEngine::NController::FrameStorage secondStorage = scheduler.GetFrameStorage(secondFrame);
    const RenderWorld& secondWorld = NRenderer::NInternal::ExtractRenderWorld(world, secondStorage);
    const NRenderer::RenderFrameIdentity secondIdentity = secondWorld.GetFrame();

    EXPECT_EQ(firstIdentity.FrameSlotIndex, secondIdentity.FrameSlotIndex);
    EXPECT_NE(firstIdentity.Generation, secondIdentity.Generation);
    EXPECT_NE(firstIdentity.ApplicationFrameIndex, secondIdentity.ApplicationFrameIndex);

    scheduler.CompleteFrame(secondFrame);
}

} // namespace
