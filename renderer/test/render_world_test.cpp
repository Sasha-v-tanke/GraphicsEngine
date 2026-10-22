#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/graphics/material.h>
#include <GraphicsEngine/renderer/render_components.h>
#include <GraphicsEngine/renderer/render_world.h>
#include <GraphicsEngine/renderer/renderer.h>
#include <GraphicsEngine/resources/resource_identity.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <engine/controller/frame_scheduler.h>
#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>
#include <renderer/extraction.h>
#include <tests/common/test_error.h>

namespace NResources {

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
using NGraphics::Material;
using NRenderer::CameraComponent;
using NRenderer::RenderableComponent;
using NRenderer::Renderer;
using NRenderer::RenderWorld;
using NRenderer::TransformComponent;
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

NResources::ResourceHandle<Material>
MakeReadyMaterial(ResourceManager& resources, ResourceIdentity identity, int revision = 0) {
    const NResources::ResourceHandle<Material> handle = resources.Request<Material>(std::move(identity));
    const NResources::ResourceOperation<Material> operation = resources.BeginLoading(handle);

    resources.PublishReady(
            operation,
            std::make_shared<Material>(ResourceIdentity{"pipeline", "material-" + std::to_string(revision)}));

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

    NTest::ExpectError(NCommon::EError::INVALID_STATE,
                       [&] { static_cast<void>(NRenderer::NInternal::ExtractRenderWorld(world, resources, storage)); });

    scheduler.SignalDraw(frame);

    NTest::ExpectError(NCommon::EError::INVALID_STATE,
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
            MakeReadyMaterial(resources, ResourceIdentity{"material", "first"}, 3);

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
    EXPECT_EQ(renderObject.Material->GetPipeline(), (ResourceIdentity{"pipeline", "material-3"}));
}

TEST(RenderWorld, RetainsResourceVersionUntilFrameRecycle) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> mesh = resources.Request<MeshData>(ResourceIdentity{"mesh", "retained"});
    const NResources::ResourceOperation<MeshData> firstMeshOperation = resources.BeginLoading(mesh);
    auto firstMeshPayload = std::make_shared<MeshData>(1);
    std::weak_ptr<const MeshData> firstMeshLifetime = firstMeshPayload;

    resources.PublishReady(firstMeshOperation, firstMeshPayload);

    const NResources::ResourceHandle<Material> material =
            MakeReadyMaterial(resources, ResourceIdentity{"material", "retained"}, 1);

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

TEST(RenderWorld, RetainsResourceVersionAcrossMultipleFrameSlots) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> mesh = resources.Request<MeshData>(ResourceIdentity{"mesh", "shared"});
    const NResources::ResourceOperation<MeshData> meshOperation = resources.BeginLoading(mesh);
    auto meshPayload = std::make_shared<MeshData>(1);
    std::weak_ptr<const MeshData> meshLifetime = meshPayload;

    resources.PublishReady(meshOperation, meshPayload);

    const NResources::ResourceHandle<Material> material =
            MakeReadyMaterial(resources, ResourceIdentity{"material", "shared"}, 1);

    NEcs::World world;
    const NEcs::Entity object = world.CreateEntity();

    world.AddComponent<TransformComponent>(object);
    world.AddComponent<RenderableComponent>(object,
                                            RenderableComponent{
                                                    .Mesh = mesh,
                                                    .Material = material,
                                            });

    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 2,
    }};

    const FrameHandle firstFrame = BeginExtractionFrame(scheduler);
    const NEngine::NController::FrameStorage firstStorage = scheduler.GetFrameStorage(firstFrame);
    const RenderWorld& firstWorld = NRenderer::NInternal::ExtractRenderWorld(world, resources, firstStorage);

    const FrameHandle secondFrame = BeginExtractionFrame(scheduler);
    const NEngine::NController::FrameStorage secondStorage = scheduler.GetFrameStorage(secondFrame);
    const RenderWorld& secondWorld = NRenderer::NInternal::ExtractRenderWorld(world, resources, secondStorage);

    ASSERT_EQ(firstWorld.GetObjects().size(), 1);
    ASSERT_EQ(secondWorld.GetObjects().size(), 1);
    ASSERT_EQ(firstWorld.GetResourceUseRecords().size(), 2U);
    ASSERT_EQ(secondWorld.GetResourceUseRecords().size(), 2U);
    EXPECT_EQ(firstWorld.GetObjects().front().Mesh.GetVersion(), secondWorld.GetObjects().front().Mesh.GetVersion());
    EXPECT_EQ(firstWorld.GetResourceUseRecords().front().GetVersion(),
              secondWorld.GetResourceUseRecords().front().GetVersion());

    meshPayload.reset();
    resources.RequestUnload(mesh);
    resources.CompleteUnload(mesh);

    EXPECT_FALSE(meshLifetime.expired());

    scheduler.CompleteFrame(firstFrame);
    scheduler.RecycleFrame(firstFrame);

    EXPECT_FALSE(meshLifetime.expired());

    scheduler.CompleteFrame(secondFrame);
    scheduler.RecycleFrame(secondFrame);

    EXPECT_TRUE(meshLifetime.expired());
}

TEST(RenderWorld, SkipsNewConsumersAfterResourceUnload) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> mesh = resources.Request<MeshData>(ResourceIdentity{"mesh", "unloaded"});
    const NResources::ResourceOperation<MeshData> meshOperation = resources.BeginLoading(mesh);

    resources.PublishReady(meshOperation, std::make_shared<MeshData>(1));

    const NResources::ResourceHandle<Material> material =
            MakeReadyMaterial(resources, ResourceIdentity{"material", "unloaded"}, 1);

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

    resources.RequestUnload(mesh);

    const FrameHandle frame = BeginExtractionFrame(scheduler);
    const NEngine::NController::FrameStorage storage = scheduler.GetFrameStorage(frame);
    const RenderWorld& renderWorld = NRenderer::NInternal::ExtractRenderWorld(world, resources, storage);

    EXPECT_TRUE(renderWorld.GetObjects().empty());
    EXPECT_TRUE(renderWorld.GetResourceUseRecords().empty());
}

TEST(RenderWorld, RejectsStalePublicResourceHandleDuringExtraction) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> staleMesh =
            MakeReadyResource<MeshData>(resources, ResourceIdentity{"mesh", "stale"}, 1);
    const NResources::ResourceHandle<Material> material =
            MakeReadyMaterial(resources, ResourceIdentity{"material", "stale"}, 1);

    resources.RequestUnload(staleMesh);
    resources.CompleteUnload(staleMesh);
    resources.Forget(staleMesh);

    static_cast<void>(resources.Request<MeshData>(ResourceIdentity{"mesh", "stale"}));

    NEcs::World world;
    const NEcs::Entity object = world.CreateEntity();

    world.AddComponent<TransformComponent>(object);
    world.AddComponent<RenderableComponent>(object,
                                            RenderableComponent{
                                                    .Mesh = staleMesh,
                                                    .Material = material,
                                            });

    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};
    const FrameHandle frame = BeginExtractionFrame(scheduler);
    const NEngine::NController::FrameStorage storage = scheduler.GetFrameStorage(frame);

    NTest::ExpectError(NCommon::EError::INVALID_STATE,
                       [&] { static_cast<void>(NRenderer::NInternal::ExtractRenderWorld(world, resources, storage)); });
}

TEST(RenderWorld, BindsSnapshotLifetimeToFrameSlotGeneration) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> mesh =
            MakeReadyResource<MeshData>(resources, ResourceIdentity{"mesh", "slot"});
    const NResources::ResourceHandle<Material> material =
            MakeReadyMaterial(resources, ResourceIdentity{"material", "slot"});

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

TEST(Renderer, ProducesNoDrawCommandsWithoutViews) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> mesh =
            MakeReadyResource<MeshData>(resources, ResourceIdentity{"mesh", "no-view"});
    const NResources::ResourceHandle<Material> material =
            MakeReadyMaterial(resources, ResourceIdentity{"material", "no-view"});

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

    const NRenderer::RenderPlan& plan = Renderer::Prepare(renderWorld, storage.GetMemoryResource());

    EXPECT_TRUE(plan.GetDrawCommands().empty());
    EXPECT_EQ(plan.GetFrame(), renderWorld.GetFrame());
}

TEST(Renderer, BuildsDrawCommandsForEveryViewObjectPair) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> mesh =
            MakeReadyResource<MeshData>(resources, ResourceIdentity{"mesh", "visible"}, 7);
    const NResources::ResourceHandle<Material> material =
            MakeReadyMaterial(resources, ResourceIdentity{"material", "visible"}, 8);

    NEcs::World world;

    const NEcs::Entity firstCamera = world.CreateEntity();
    world.AddComponent<TransformComponent>(firstCamera);
    world.AddComponent<CameraComponent>(firstCamera);

    const NEcs::Entity secondCamera = world.CreateEntity();
    world.AddComponent<TransformComponent>(secondCamera);
    world.AddComponent<CameraComponent>(secondCamera);

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

    const NRenderer::RenderPlan& plan = Renderer::Prepare(renderWorld, storage.GetMemoryResource());

    ASSERT_EQ(plan.GetDrawCommands().size(), 2);
    EXPECT_EQ(plan.GetDrawCommands()[0].ViewId.Index, firstCamera.Index);
    EXPECT_EQ(plan.GetDrawCommands()[1].ViewId.Index, secondCamera.Index);

    for (const NRenderer::DrawCommandData& command: plan.GetDrawCommands()) {
        EXPECT_EQ(command.ObjectId.Index, object.Index);
        EXPECT_FLOAT_EQ(command.WorldTransform.Translation.X, 1.0F);
        EXPECT_EQ(command.Mesh, mesh);
        EXPECT_EQ(command.MeshVersion, renderObject.Mesh.GetVersion());
        EXPECT_EQ(command.Material, material);
        EXPECT_EQ(command.MaterialVersion, renderObject.Material.GetVersion());
    }
}

TEST(Renderer, SortsDrawCommandsDeterministically) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> firstMesh =
            MakeReadyResource<MeshData>(resources, ResourceIdentity{"mesh", "a"}, 1);
    const NResources::ResourceHandle<MeshData> secondMesh =
            MakeReadyResource<MeshData>(resources, ResourceIdentity{"mesh", "b"}, 2);
    const NResources::ResourceHandle<Material> firstMaterial =
            MakeReadyMaterial(resources, ResourceIdentity{"material", "a"}, 3);
    const NResources::ResourceHandle<Material> secondMaterial =
            MakeReadyMaterial(resources, ResourceIdentity{"material", "b"}, 4);

    NEcs::World world;

    const NEcs::Entity camera = world.CreateEntity();
    world.AddComponent<TransformComponent>(camera);
    world.AddComponent<CameraComponent>(camera);

    const NEcs::Entity secondObject = world.CreateEntity();
    world.AddComponent<TransformComponent>(secondObject);
    world.AddComponent<RenderableComponent>(secondObject,
                                            RenderableComponent{
                                                    .Mesh = secondMesh,
                                                    .Material = secondMaterial,
                                            });

    const NEcs::Entity firstObject = world.CreateEntity();
    world.AddComponent<TransformComponent>(firstObject);
    world.AddComponent<RenderableComponent>(firstObject,
                                            RenderableComponent{
                                                    .Mesh = firstMesh,
                                                    .Material = firstMaterial,
                                            });

    FrameScheduler scheduler{EngineConfig{
            .MaxActiveFrames = 1,
    }};
    const FrameHandle frame = BeginExtractionFrame(scheduler);
    const NEngine::NController::FrameStorage storage = scheduler.GetFrameStorage(frame);
    const RenderWorld& renderWorld = NRenderer::NInternal::ExtractRenderWorld(world, resources, storage);

    const NRenderer::RenderPlan& firstPlan = Renderer::Prepare(renderWorld, storage.GetMemoryResource());
    const NRenderer::RenderPlan& secondPlan = Renderer::Prepare(renderWorld, storage.GetMemoryResource());

    ASSERT_EQ(firstPlan.GetDrawCommands().size(), 2);
    ASSERT_EQ(secondPlan.GetDrawCommands().size(), 2);

    EXPECT_EQ(firstPlan.GetDrawCommands()[0].ObjectId.Index, firstObject.Index);
    EXPECT_EQ(firstPlan.GetDrawCommands()[0].ObjectId.Generation, firstObject.Generation);
    EXPECT_EQ(firstPlan.GetDrawCommands()[1].ObjectId.Index, secondObject.Index);
    EXPECT_EQ(firstPlan.GetDrawCommands()[1].ObjectId.Generation, secondObject.Generation);
    EXPECT_EQ(secondPlan.GetDrawCommands()[0].ObjectId, firstPlan.GetDrawCommands()[0].ObjectId);
    EXPECT_EQ(secondPlan.GetDrawCommands()[1].ObjectId, firstPlan.GetDrawCommands()[1].ObjectId);
}

TEST(Renderer, UsesObjectIdAsFinalSortTieBreak) {
    ResourceManager resources;
    const NResources::ResourceHandle<MeshData> mesh =
            MakeReadyResource<MeshData>(resources, ResourceIdentity{"mesh", "tie"}, 1);
    const NResources::ResourceHandle<Material> material =
            MakeReadyMaterial(resources, ResourceIdentity{"material", "tie"}, 2);

    NEcs::World world;

    const NEcs::Entity camera = world.CreateEntity();
    world.AddComponent<TransformComponent>(camera);
    world.AddComponent<CameraComponent>(camera);

    const NEcs::Entity firstObject = world.CreateEntity();
    world.AddComponent<TransformComponent>(firstObject);
    world.AddComponent<RenderableComponent>(firstObject,
                                            RenderableComponent{
                                                    .Mesh = mesh,
                                                    .Material = material,
                                            });

    const NEcs::Entity secondObject = world.CreateEntity();
    world.AddComponent<TransformComponent>(secondObject);
    world.AddComponent<RenderableComponent>(secondObject,
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

    const NRenderer::RenderPlan& plan = Renderer::Prepare(renderWorld, storage.GetMemoryResource());

    ASSERT_EQ(plan.GetDrawCommands().size(), 2);
    EXPECT_EQ(plan.GetDrawCommands()[0].ObjectId.Index, firstObject.Index);
    EXPECT_EQ(plan.GetDrawCommands()[0].ObjectId.Generation, firstObject.Generation);
    EXPECT_EQ(plan.GetDrawCommands()[1].ObjectId.Index, secondObject.Index);
    EXPECT_EQ(plan.GetDrawCommands()[1].ObjectId.Generation, secondObject.Generation);
}

const RenderWorld* ExtractParallel(const NEcs::World& world,
                                   const ResourceManager& resources,
                                   const NEngine::NController::FrameStorage& storage,
                                   NCommon::TaskSystem& tasks,
                                   std::size_t chunkSize) {
    const RenderWorld* result = nullptr;
    NCommon::TaskHandle join;
    const auto root = tasks.Submit([&](NCommon::TaskContext& task) {
        join = NRenderer::NInternal::ScheduleRenderWorldExtraction(
                world,
                resources,
                storage,
                task,
                [&](const RenderWorld& snapshot) { result = &snapshot; },
                chunkSize);
    });
    tasks.Wait(root);
    EXPECT_EQ(tasks.GetStatus(root), NCommon::ETaskStatus::COMPLETED);
    if (!join.IsValid()) {
        return nullptr;
    }
    tasks.Wait(join);
    EXPECT_EQ(tasks.GetStatus(join), NCommon::ETaskStatus::COMPLETED);
    return result;
}

TEST(RenderWorld, ParallelExtractionMatchesSerialAcrossUnevenChunksAndFrameSlots) {
    ResourceManager resources;
    const auto mesh = MakeReadyResource<MeshData>(resources, {"mesh", "parallel"}, 7);
    const auto material = MakeReadyMaterial(resources, {"material", "parallel"}, 8);
    NEcs::World world;
    for (int index = 0; index < 1031; ++index) {
        const auto entity = world.CreateEntity();
        world.AddComponent<TransformComponent>(entity).Value.Translation.X = static_cast<float>(index);
        if (index % 13 == 0) {
            world.AddComponent<CameraComponent>(entity);
        }
        world.AddComponent<RenderableComponent>(
                entity,
                RenderableComponent{
                        .Mesh = index % 11 == 0 ? NResources::ResourceHandle<MeshData>{} : mesh,
                        .Material = material,
                });
    }
    FrameScheduler scheduler{EngineConfig{.MaxActiveFrames = 5}};
    const auto serialFrame = BeginExtractionFrame(scheduler);
    const auto& serial =
            NRenderer::NInternal::ExtractRenderWorld(world, resources, scheduler.GetFrameStorage(serialFrame));
    NCommon::TaskSystem tasks{4};
    for (const std::size_t chunkSize: {17U, 31U, 256U, 2048U}) {
        const auto parallelFrame = BeginExtractionFrame(scheduler);
        const auto* parallel =
                ExtractParallel(world, resources, scheduler.GetFrameStorage(parallelFrame), tasks, chunkSize);
        ASSERT_NE(parallel, nullptr);
        ASSERT_EQ(parallel->GetViews().size(), serial.GetViews().size());
        ASSERT_EQ(parallel->GetObjects().size(), serial.GetObjects().size());
        EXPECT_EQ(parallel->GetResourceUseRecords().size(), serial.GetResourceUseRecords().size());
        for (std::size_t index = 0; index < serial.GetViews().size(); ++index) {
            EXPECT_EQ(parallel->GetViews()[index].Id, serial.GetViews()[index].Id);
            EXPECT_EQ(parallel->GetViews()[index].WorldTransform.Translation.X,
                      serial.GetViews()[index].WorldTransform.Translation.X);
        }
        for (std::size_t index = 0; index < serial.GetObjects().size(); ++index) {
            const auto& expected = serial.GetObjects()[index];
            const auto& actual = parallel->GetObjects()[index];
            EXPECT_EQ(actual.Id, expected.Id);
            EXPECT_EQ(actual.WorldTransform.Translation.X, expected.WorldTransform.Translation.X);
            EXPECT_EQ(actual.Mesh.GetVersion(), expected.Mesh.GetVersion());
            EXPECT_EQ(actual.Material.GetVersion(), expected.Material.GetVersion());
        }
        EXPECT_NE(parallel->GetFrame(), serial.GetFrame());
        scheduler.AbortFrame(parallelFrame);
    }
}

TEST(RenderWorld, ParallelExtractionJoinsEmptyWorldWithOneWorker) {
    FrameScheduler scheduler{EngineConfig{.MaxActiveFrames = 1}};
    ResourceManager resources;
    NEcs::World world;
    NCommon::TaskSystem tasks{1};
    const auto frame = BeginExtractionFrame(scheduler);
    const auto* snapshot = ExtractParallel(world, resources, scheduler.GetFrameStorage(frame), tasks, 256);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(snapshot->GetObjects().empty());
    EXPECT_TRUE(snapshot->GetViews().empty());
}

TEST(RenderWorld, ParallelExtractionDoesNotPublishOnChunkFailure) {
    FrameScheduler scheduler{EngineConfig{.MaxActiveFrames = 1}};
    ResourceManager resources;
    ResourceManager otherResources;
    const auto foreignMesh = MakeReadyResource<MeshData>(otherResources, {"mesh", "foreign"});
    const auto material = MakeReadyMaterial(resources, {"material", "valid"});
    NEcs::World world;
    for (int index = 0; index < 1031; ++index) {
        const auto entity = world.CreateEntity();
        world.AddComponent<TransformComponent>(entity);
        world.AddComponent<RenderableComponent>(entity, RenderableComponent{.Mesh = foreignMesh, .Material = material});
    }
    NCommon::TaskSystem tasks{4};
    const auto frame = BeginExtractionFrame(scheduler);
    bool published = false;
    NCommon::TaskHandle join;
    const auto root = tasks.Submit([&](NCommon::TaskContext& task) {
        join = NRenderer::NInternal::ScheduleRenderWorldExtraction(
                world,
                resources,
                scheduler.GetFrameStorage(frame),
                task,
                [&](const RenderWorld&) { published = true; },
                17);
    });
    tasks.Wait(root);
    ASSERT_TRUE(join.IsValid());
    tasks.Wait(join);
    EXPECT_EQ(tasks.GetStatus(join), NCommon::ETaskStatus::FAILED);
    EXPECT_FALSE(published);
    // The failed join has drained every chunk, so mutable World access is safe again.
    world.DestroyEntity({.Index = 0, .Generation = 1});
}

class ParallelExtractionRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    ParallelExtractionRuntime() {
        for (int index = 0; index < 1031; ++index) {
            const auto entity = World.CreateEntity();
            World.AddComponent<TransformComponent>(entity);
            World.AddComponent<CameraComponent>(entity);
        }
    }

    void Update(const NEngine::NRuntime::FrameContext&) override {
        ++Updates;
    }

    NCommon::TaskHandle ScheduleExtract(const NEngine::NRuntime::FrameContext& frame,
                                        NCommon::TaskContext& task) override {
        return NRenderer::NInternal::ScheduleRenderWorldExtraction(
                World,
                Resources,
                frame.GetStorage(),
                task,
                [this](const RenderWorld& snapshot) {
                    Snapshot = &snapshot;
                    Published.set_value();
                    if (Release.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
                        throw std::runtime_error{"Extraction test release timed out"};
                    }
                    if (FailAfterJoin) {
                        throw std::runtime_error{"Extraction publication failed"};
                    }
                },
                17);
    }

    void Draw(const NEngine::NRuntime::FrameContext&) override {
        EXPECT_EQ(Snapshot->GetViews().size(), 1031);
        ++Draws;
    }

    NEcs::World World;
    ResourceManager Resources;
    std::promise<void> Published;
    std::promise<void> ReleasePromise;
    std::shared_future<void> Release = ReleasePromise.get_future().share();
    std::atomic<int> Updates{0};
    std::atomic<int> Draws{0};
    const RenderWorld* Snapshot = nullptr;
    bool FailAfterJoin = false;
};

TEST(RenderWorld, EngineKeepsWorldClosedUntilParallelExtractionJoinCompletes) {
    auto runtime = std::make_unique<ParallelExtractionRuntime>();
    auto& runtimeRef = *runtime;
    auto published = runtimeRef.Published.get_future();
    auto engine = NEngine::NRuntime::EngineFactory::Create(EngineConfig{.MaxActiveFrames = 2, .WorkerCount = 1},
                                                           std::move(runtime));
    engine->Start();
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(published.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_FALSE(engine->Update());
    EXPECT_EQ(runtimeRef.Updates.load(), 1);
    EXPECT_EQ(runtimeRef.Draws.load(), 0);
    runtimeRef.ReleasePromise.set_value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool admitted = false;
    while (!admitted && std::chrono::steady_clock::now() < deadline) {
        admitted = engine->Update();
        std::this_thread::yield();
    }
    EXPECT_TRUE(admitted);
    engine->Stop();
    EXPECT_EQ(runtimeRef.Updates.load(), 2);
    EXPECT_EQ(runtimeRef.Draws.load(), 1);
    EXPECT_FALSE(engine->GetLastError().has_value());
}

TEST(RenderWorld, EngineDrainsFailedExtractionWithoutDrawingOrReopeningWorld) {
    auto runtime = std::make_unique<ParallelExtractionRuntime>();
    auto& runtimeRef = *runtime;
    runtimeRef.FailAfterJoin = true;
    auto published = runtimeRef.Published.get_future();
    auto engine = NEngine::NRuntime::EngineFactory::Create(EngineConfig{.MaxActiveFrames = 2, .WorkerCount = 4},
                                                           std::move(runtime));
    engine->Start();
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_EQ(published.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_FALSE(engine->Update());
    runtimeRef.ReleasePromise.set_value();
    engine->Stop();
    EXPECT_EQ(runtimeRef.Updates.load(), 1);
    EXPECT_EQ(runtimeRef.Draws.load(), 0);
    ASSERT_TRUE(engine->GetLastError().has_value());
    EXPECT_EQ(engine->GetLastError()->Message, "Extraction publication failed");
}

TEST(RenderWorld, FailedSchedulingRootNeverStartsChunksOrPublishes) {
    FrameScheduler scheduler{EngineConfig{.MaxActiveFrames = 1}};
    ResourceManager resources;
    NEcs::World world;
    const auto entity = world.CreateEntity();
    world.AddComponent<TransformComponent>(entity);
    world.AddComponent<CameraComponent>(entity);
    NCommon::TaskSystem tasks{4};
    const auto frame = BeginExtractionFrame(scheduler);
    bool published = false;
    NCommon::TaskHandle join;
    const auto root = tasks.Submit([&](NCommon::TaskContext& task) {
        join = NRenderer::NInternal::ScheduleRenderWorldExtraction(world,
                                                                   resources,
                                                                   scheduler.GetFrameStorage(frame),
                                                                   task,
                                                                   [&](const RenderWorld&) { published = true; });
        throw std::runtime_error{"Scheduling failed after graph submission"};
    });
    tasks.Wait(root);
    ASSERT_TRUE(join.IsValid());
    tasks.Wait(join);
    EXPECT_EQ(tasks.GetStatus(join), NCommon::ETaskStatus::CANCELLED);
    EXPECT_FALSE(published);
    world.DestroyEntity(entity);
}

TEST(RenderWorld, ParallelExtractionStressPreservesPreviousSlotSnapshot) {
    FrameScheduler scheduler{EngineConfig{.MaxActiveFrames = 2}};
    ResourceManager resources;
    NEcs::World world;
    for (int index = 0; index < 1003; ++index) {
        const auto entity = world.CreateEntity();
        world.AddComponent<TransformComponent>(entity);
        world.AddComponent<CameraComponent>(entity);
    }
    NCommon::TaskSystem tasks{4};
    const RenderWorld* previous = nullptr;
    FrameHandle previousFrame;
    for (int iteration = 0; iteration < 40; ++iteration) {
        world.Query<TransformComponent>([&](NEcs::Entity, TransformComponent& transform) {
            transform.Value.Translation.X = static_cast<float>(iteration);
        });
        const auto frame = BeginExtractionFrame(scheduler);
        const auto* snapshot = ExtractParallel(world, resources, scheduler.GetFrameStorage(frame), tasks, 64);
        ASSERT_NE(snapshot, nullptr);
        ASSERT_EQ(snapshot->GetViews().size(), 1003);
        for (std::size_t index = 0; index < snapshot->GetViews().size(); ++index) {
            EXPECT_EQ(snapshot->GetViews()[index].Id.Index, index);
            EXPECT_EQ(snapshot->GetViews()[index].WorldTransform.Translation.X, static_cast<float>(iteration));
        }
        if (previous != nullptr) {
            EXPECT_EQ(previous->GetViews()[0].WorldTransform.Translation.X, static_cast<float>(iteration - 1));
            scheduler.AbortFrame(previousFrame);
        }
        previous = snapshot;
        previousFrame = frame;
    }
}

} // namespace
