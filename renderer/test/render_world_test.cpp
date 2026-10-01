#include <memory>
#include <optional>
#include <utility>

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

class Material final {
public:
    explicit Material(int revision = 0) noexcept
        : Revision(revision) {
    }

    int Revision = 0;
};

class MeshData final {
public:
    explicit MeshData(int revision = 0) noexcept
        : Revision(revision) {
    }

    int Revision = 0;
};

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

template<typename T>
NResources::ResourceHandle<T>
MakeReadyResource(ResourceManager& resources, ResourceIdentity identity, int revision = 0) {
    const NResources::ResourceHandle<T> handle = resources.Request<T>(std::move(identity));
    const NResources::ResourceOperation<T> operation = resources.BeginLoading(handle);

    resources.PublishReady(operation, std::make_shared<T>(revision));

    return handle;
}

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
    ResourceManager resources;
    NEcs::World world;

    const FrameHandle frame = *scheduler.TryAcquireFrame();
    scheduler.ArmFrame(frame);
    scheduler.BeginUpdate(frame);
    scheduler.EndUpdate(frame);

    const NEngine::NController::FrameStorage storage = scheduler.GetFrameStorage(frame);

    NTest::ExpectError(
            NCommon::EError::INVALID_STATE,
            [&] { static_cast<void>(NRenderer::NInternal::ExtractRenderWorld(world, resources, storage)); });

    scheduler.SignalDraw(frame);

    NTest::ExpectError(
            NCommon::EError::INVALID_STATE,
            [&] { static_cast<void>(NRenderer::NInternal::ExtractRenderWorld(world, resources, storage)); });

    scheduler.BeginFinalize(frame);

    const RenderWorld& renderWorld = NRenderer::NInternal::ExtractRenderWorld(world, resources, storage);

    EXPECT_TRUE(renderWorld.GetViews().empty());
    EXPECT_TRUE(renderWorld.GetObjects().empty());
}

TEST(RenderWorld, IsolatesWorldAndResourceMutationsAfterExtraction) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> firstMesh =
            MakeReadyResource<MeshData>(resources, ResourceIdentity{"mesh", "first"}, 1);
    const NResources::ResourceHandle<MeshData> secondMesh =
            MakeReadyResource<MeshData>(resources, ResourceIdentity{"mesh", "second"}, 2);
    const NResources::ResourceHandle<Material> material =
            MakeReadyResource<Material>(resources, ResourceIdentity{"material", "first"}, 3);

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
    const RenderWorld& renderWorld = NRenderer::NInternal::ExtractRenderWorld(world, resources, storage);

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
    EXPECT_EQ(renderObject.Mesh.GetResource(), firstMesh);
    EXPECT_EQ(renderObject.Material.GetResource(), material);
    EXPECT_EQ(renderObject.Mesh->Revision, 1);
    EXPECT_EQ(renderObject.Material->Revision, 3);
}

TEST(RenderWorld, RetainsResourceVersionUntilFrameRecycle) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> mesh = resources.Request<MeshData>(ResourceIdentity{"mesh", "retained"});
    const NResources::ResourceOperation<MeshData> firstMeshOperation = resources.BeginLoading(mesh);
    auto firstMeshPayload = std::make_shared<MeshData>(1);
    std::weak_ptr<const MeshData> firstMeshLifetime = firstMeshPayload;

    resources.PublishReady(firstMeshOperation, firstMeshPayload);

    const NResources::ResourceHandle<Material> material =
            MakeReadyResource<Material>(resources, ResourceIdentity{"material", "retained"}, 1);

    NEcs::World world;
    const NEcs::Entity object = world.CreateEntity();

    world.AddComponent<TransformComponent>(object);
    world.AddComponent<RenderableComponent>(object,
                                            RenderableComponent{
                                                    .Mesh = mesh,
                                                    .Material = material,
                                            });

    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};
    const FrameHandle frame = BeginExtractionFrame(scheduler);
    const NEngine::NController::FrameStorage storage = scheduler.GetFrameStorage(frame);
    const RenderWorld& renderWorld = NRenderer::NInternal::ExtractRenderWorld(world, resources, storage);

    ASSERT_EQ(renderWorld.GetObjects().size(), 1);

    const NRenderer::RenderObject& renderObject = renderWorld.GetObjects().front();
    const std::uint64_t retainedVersion = renderObject.Mesh.GetVersion();

    EXPECT_EQ(renderObject.Mesh.GetResource(), mesh);
    EXPECT_EQ(renderObject.Mesh->Revision, 1);

    firstMeshPayload.reset();

    resources.RequestUnload(mesh);

    EXPECT_FALSE(resources.TryAcquire(mesh).has_value());
    EXPECT_FALSE(firstMeshLifetime.expired());

    resources.CompleteUnload(mesh);

    const NResources::ResourceOperation<MeshData> secondMeshOperation = resources.BeginLoading(mesh);

    resources.PublishReady(secondMeshOperation, std::make_shared<MeshData>(2));

    std::optional<NResources::ResourceLease<MeshData>> currentMesh = resources.TryAcquire(mesh);

    ASSERT_TRUE(currentMesh.has_value());
    EXPECT_NE(currentMesh->GetVersion(), retainedVersion);
    EXPECT_EQ((*currentMesh)->Revision, 2);

    EXPECT_EQ(renderObject.Mesh.GetVersion(), retainedVersion);
    EXPECT_EQ(renderObject.Mesh->Revision, 1);
    EXPECT_FALSE(firstMeshLifetime.expired());

    scheduler.CompleteFrame(frame);
    scheduler.RecycleFrame(frame);

    EXPECT_TRUE(firstMeshLifetime.expired());
}

TEST(RenderWorld, BindsSnapshotLifetimeToFrameSlotGeneration) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> mesh =
            MakeReadyResource<MeshData>(resources, ResourceIdentity{"mesh", "slot"});
    const NResources::ResourceHandle<Material> material =
            MakeReadyResource<Material>(resources, ResourceIdentity{"material", "slot"});

    NEcs::World world;

    const NEcs::Entity object = world.CreateEntity();
    world.AddComponent<TransformComponent>(object);
    world.AddComponent<RenderableComponent>(object,
                                            RenderableComponent{
                                                    .Mesh = mesh,
                                                    .Material = material,
                                            });

    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};

    const FrameHandle firstFrame = BeginExtractionFrame(scheduler);
    const NEngine::NController::FrameStorage firstStorage = scheduler.GetFrameStorage(firstFrame);
    const RenderWorld& firstWorld = NRenderer::NInternal::ExtractRenderWorld(world, resources, firstStorage);
    const NRenderer::RenderFrameIdentity firstIdentity = firstWorld.GetFrame();

    ASSERT_EQ(firstWorld.GetObjects().size(), 1);

    scheduler.CompleteFrame(firstFrame);
    scheduler.RecycleFrame(firstFrame);

    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(firstStorage.GetState()); });

    const FrameHandle secondFrame = BeginExtractionFrame(scheduler);
    const NEngine::NController::FrameStorage secondStorage = scheduler.GetFrameStorage(secondFrame);
    const RenderWorld& secondWorld = NRenderer::NInternal::ExtractRenderWorld(world, resources, secondStorage);
    const NRenderer::RenderFrameIdentity secondIdentity = secondWorld.GetFrame();

    EXPECT_EQ(firstIdentity.FrameSlotIndex, secondIdentity.FrameSlotIndex);
    EXPECT_NE(firstIdentity.Generation, secondIdentity.Generation);
    EXPECT_NE(firstIdentity.ApplicationFrameIndex, secondIdentity.ApplicationFrameIndex);

    scheduler.CompleteFrame(secondFrame);
    scheduler.RecycleFrame(secondFrame);
}

} // namespace
