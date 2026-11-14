#include <array>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <ecs/internal/scoped_batch.h>
#include <ecs/internal/world_changes.h>
#include <engine/controller/frame_scheduler.h>
#include <graphics/material.h>
#include <gtest/gtest.h>
#include <renderer/extraction.h>
#include <renderer/renderer.h>
#include <resources/mesh_data.h>

namespace {

using NRenderer::CameraComponent;
using NRenderer::RenderableComponent;
using NRenderer::TransformComponent;
using NRenderer::NInternal::IncrementalExtraction;

class Incremental: public testing::TestWithParam<std::pair<std::size_t, std::size_t>> {
protected:
    NResources::ResourceManager Resources;
    NEcs::World World;
    std::unique_ptr<NCommon::TaskSystem> Tasks;
    std::unique_ptr<NEngine::NController::FrameScheduler> Frames;
    std::vector<NEngine::NController::FrameHandle> Active;
    IncrementalExtraction Cache;
    NResources::ResourceHandle<NResources::MeshData> Mesh;
    NResources::ResourceHandle<NGraphics::Material> Material;
    NEcs::Entity Camera;
    std::vector<NEcs::Entity> Objects;

    void SetUp() override {
        Tasks = std::make_unique<NCommon::TaskSystem>(GetParam().first);
        Frames = std::make_unique<NEngine::NController::FrameScheduler>(
                NEngine::EngineConfig{.MaxActiveFrames = GetParam().second});
        Mesh = Resources.Request<NResources::MeshData>({"mesh", "incremental"});
        Resources.PublishReady(Resources.BeginLoading(Mesh), std::make_shared<NResources::MeshData>());
        Material = Resources.Request<NGraphics::Material>({"material", "incremental"});
        PublishMaterial();
        Camera = World.CreateEntity();
        World.AddComponent<TransformComponent>(Camera);
        World.AddComponent<CameraComponent>(Camera);
        for (std::size_t index = 0; index < 5; ++index) {
            AddObject();
        }
    }

    void PublishMaterial() {
        Resources.PublishReady(
                Resources.BeginLoading(Material),
                std::make_shared<NGraphics::Material>(NResources::ResourceIdentity{"pipeline", "incremental"}));
    }

    void AddObject() {
        const auto entity = World.CreateEntity();
        World.AddComponent<TransformComponent>(entity, TransformComponent{{.Translation = {1, 0, 0}}});
        World.AddComponent<RenderableComponent>(entity, RenderableComponent{.Mesh = Mesh, .Material = Material});
        Objects.push_back(entity);
    }

    NEngine::NController::FrameHandle Begin() {
        if (Active.size() == GetParam().second) {
            Frames->CompleteFrame(Active.front());
            Frames->RecycleFrame(Active.front());
            Active.erase(Active.begin());
        }
        const auto frame = *Frames->TryAcquireFrame();
        Frames->ArmFrame(frame);
        Frames->BeginUpdate(frame);
        Frames->EndUpdate(frame);
        Frames->SignalDraw(frame);
        Frames->BeginFinalize(frame);
        Active.push_back(frame);
        return frame;
    }

    const NRenderer::RenderWorld& Extract(NEcs::World* source = nullptr) {
        const auto frame = Begin();
        const NRenderer::RenderWorld* result = nullptr;
        NCommon::TaskHandle join;
        const auto root = Tasks->Submit([&](NCommon::TaskContext& task) {
            join = Cache.Schedule(source ? *source : World,
                                  Resources,
                                  Frames->GetFrameStorage(frame),
                                  *Tasks,
                                  task,
                                  [&](const auto& snapshot) { result = &snapshot; });
        });
        Tasks->Wait(root);
        if (Tasks->GetStatus(root) != NCommon::ETaskStatus::COMPLETED) {
            throw std::runtime_error(Tasks->GetError(root)->Message);
        }
        Tasks->Wait(join);
        if (Tasks->GetStatus(join) != NCommon::ETaskStatus::COMPLETED) {
            throw std::runtime_error(Tasks->GetError(join)->Message);
        }
        Tasks->WaitIdle();
        return *result;
    }
};

TEST_P(Incremental, UnchangedFrameSharesBackingAndScopedWriteChangesOneObject) {
    const auto& first = Extract();
    EXPECT_TRUE(first.GetDelta().FullRebuild);
    const auto* backing = first.GetObjects().data();
    const auto& second = Extract();
    EXPECT_FALSE(second.GetDelta().FullRebuild);
    EXPECT_EQ(second.GetObjects().data(), backing);
    EXPECT_TRUE(second.GetDelta().UpdatedObjects.empty());
    EXPECT_EQ(Cache.GetStatistics().RecomputedTransforms, 0U);
    EXPECT_EQ(Cache.GetStatistics().ResourceObservations, 2U);
    const auto changed = World.GetEntityReference(Objects[2]);
    NEcs::NInternal::ScopedWorldAccess::Register(
            World,
            {NEcs::NInternal::ScopedWrite<TransformComponent>(NEcs::NInternal::BatchScope::Entity(changed))},
            [changed](NEcs::NInternal::ScopedBatchContext& context) {
                context.Write<TransformComponent>().Get(changed).Value.Translation.X = 7;
            },
            NEcs::SystemSchedule{.Id = 1});
    World.RunSystems(*Tasks);
    const auto& third = Extract();
    ASSERT_EQ(third.GetDelta().UpdatedObjects.size(), 1U);
    EXPECT_EQ(third.GetDelta().UpdatedObjects[0].Index, changed.Identity.Index);
    EXPECT_FALSE(third.GetDelta().FullRebuild);
    EXPECT_FLOAT_EQ((*third.GetObjects()[2].ResolvedWorldTransform)(0, 3), 7);
    EXPECT_EQ(Cache.GetStatistics().UpdatedObjects, 1U);
    if (GetParam().second > 1) {
        EXPECT_FLOAT_EQ((*second.GetObjects()[2].ResolvedWorldTransform)(0, 3), 1);
    }
}

TEST_P(Incremental, ParentMatrixAndShearReachDrawCommandsWithoutTrsDecomposition) {
    const auto parent = World.GetEntityReference(World.CreateEntity());
    const auto child = World.GetEntityReference(Objects[0]);
    World.DeferReparent(child, parent);
    World.ApplyDeferredStructuralChanges();
    auto matrix = NMath::Mat4::Identity();
    matrix(0, 1) = 0.5F;
    matrix(0, 3) = 5;
    World.SetLocalTransform(parent, matrix);
    const auto& first = Extract();
    ASSERT_TRUE(first.GetObjects()[0].ResolvedWorldTransform);
    EXPECT_FLOAT_EQ((*first.GetObjects()[0].ResolvedWorldTransform)(0, 1), 0.5F);
    EXPECT_FLOAT_EQ((*first.GetObjects()[0].ResolvedWorldTransform)(0, 3), 6);
    matrix(0, 3) = 10;
    World.SetLocalTransform(parent, matrix);
    const auto& second = Extract();
    ASSERT_EQ(second.GetDelta().UpdatedObjects.size(), 1U);
    EXPECT_FLOAT_EQ((*second.GetObjects()[0].ResolvedWorldTransform)(0, 3), 11);
    const auto& plan = NRenderer::Renderer::Prepare(second, Frames->GetFrameStorage(Active.back()).GetMemoryResource());
    ASSERT_EQ(plan.GetDrawCommands().size(), Objects.size());
    ASSERT_TRUE(plan.GetDrawCommands()[0].ResolvedWorldTransform);
    EXPECT_FLOAT_EQ((*plan.GetDrawCommands()[0].ResolvedWorldTransform)(0, 1), 0.5F);
    EXPECT_FLOAT_EQ((*plan.GetDrawCommands()[0].ResolvedWorldTransform)(0, 3), 11);
    if (GetParam().second > 1) {
        EXPECT_FLOAT_EQ((*first.GetObjects()[0].ResolvedWorldTransform)(0, 3), 6);
    }
}

TEST_P(Incremental, ReadinessRetryAndHotReloadPinOldVersionsAcrossFrames) {
    for (unsigned version = 1; version < 5; ++version) {
        Resources.RequestUnload(Material);
        Resources.CompleteUnload(Material);
        PublishMaterial();
    }
    const auto& first = Extract();
    EXPECT_EQ(first.GetObjects()[0].Material.GetVersion(), 5U);
    Resources.RequestUnload(Material);
    Resources.CompleteUnload(Material);
    Resources.Fail(Resources.BeginLoading(Material), {.Code = NCommon::EError::IO_ERROR, .Message = "material failed"});
    const auto& failed = Extract();
    EXPECT_TRUE(failed.GetObjects().empty());
    EXPECT_EQ(failed.GetDelta().RemovedObjects.size(), Objects.size());
    EXPECT_EQ(failed.GetResourceDiagnostics().size(), Objects.size());
    PublishMaterial();
    const auto& reloaded = Extract();
    ASSERT_EQ(reloaded.GetObjects().size(), Objects.size());
    EXPECT_EQ(reloaded.GetObjects()[0].Material.GetVersion(), 6U);
    EXPECT_EQ(reloaded.GetDelta().UpdatedObjects.size(), Objects.size());
    EXPECT_FLOAT_EQ((*reloaded.GetObjects()[0].ResolvedWorldTransform)(0, 3), 1);
    if (GetParam().second > 1) {
        EXPECT_EQ(failed.GetResourceDiagnostics()[0].Source.Failure->Message, "material failed");
    }
    if (GetParam().second > 2) {
        EXPECT_EQ(first.GetObjects()[0].Material.GetVersion(), 5U);
    }
    Resources.RequestUnload(Material);
    Resources.CompleteUnload(Material);
    Resources.Forget(Material);
    Cache.Reset();
    EXPECT_EQ(reloaded.GetObjects()[0].Material.GetVersion(), 6U);
    EXPECT_NE(reloaded.GetObjects()[0].Material.Get(), nullptr);
}

TEST_P(Incremental, CreateDestroyRecycleMissingHistoryAndWorldSwitchRebuildSafely) {
    (void)Extract();
    const auto old = Objects.front();
    World.DestroyEntity(old);
    AddObject();
    const auto replacement = Objects.back();
    EXPECT_EQ(replacement.Index, old.Index);
    EXPECT_NE(replacement.Generation, old.Generation);
    const auto& recycled = Extract();
    EXPECT_TRUE(recycled.GetDelta().FullRebuild);
    EXPECT_EQ(recycled.GetDelta().RemovedObjects.size(), 1U);
    EXPECT_EQ(recycled.GetDelta().RemovedObjects[0].Generation, old.Generation);
    for (unsigned index = 0; index < 40; ++index) {
        World.GetComponent<TransformComponent>(replacement).Value.Translation.X = static_cast<float>(index);
        (void)NEcs::NInternal::WorldChangeAccess::PublishEpoch(World);
    }
    const auto& rebuilt = Extract();
    EXPECT_TRUE(rebuilt.GetDelta().FullRebuild);
    EXPECT_FLOAT_EQ((*rebuilt.GetObjects().front().ResolvedWorldTransform)(0, 3), 39);
    NEcs::World other;
    const auto& empty = Extract(&other);
    EXPECT_TRUE(empty.GetDelta().FullRebuild);
    EXPECT_TRUE(empty.GetObjects().empty());
    EXPECT_TRUE(empty.GetViews().empty());
    if (GetParam().second > 1) {
        EXPECT_EQ(rebuilt.GetObjects().size(), 5U);
    }
}

TEST_P(Incremental, FailureDoesNotAdvanceCacheAndReadinessIsLatchedAtCapture) {
    (void)Extract();
    World.GetComponent<RenderableComponent>(Objects.front()).ReadinessPolicy =
            static_cast<NRenderer::EResourceReadinessPolicy>(99);
    EXPECT_THROW((void)Extract(), std::runtime_error);
    World.GetComponent<RenderableComponent>(Objects.front()).ReadinessPolicy =
            NRenderer::EResourceReadinessPolicy::SKIP_OBJECT;
    ASSERT_EQ(Extract().GetObjects().size(), Objects.size());
    Resources.RequestUnload(Material);
    Resources.CompleteUnload(Material);
    const auto operation = Resources.BeginLoading(Material);
    const auto frame = Begin();
    const NRenderer::RenderWorld* result = nullptr;
    NCommon::TaskHandle join;
    const auto root = Tasks->Submit([&](NCommon::TaskContext& task) {
        join = Cache.Schedule(World,
                              Resources,
                              Frames->GetFrameStorage(frame),
                              *Tasks,
                              task,
                              [&](const auto& snapshot) { result = &snapshot; });
        Resources.PublishReady(operation,
                               std::make_shared<NGraphics::Material>(NResources::ResourceIdentity{"pipeline", "late"}));
    });
    Tasks->Wait(root);
    ASSERT_TRUE(join.IsValid());
    Tasks->Wait(join);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->GetObjects().empty());
    EXPECT_EQ(Extract().GetObjects().size(), Objects.size());
}

TEST_P(Incremental, CancelledPublicationKeepsPreviousCursorAndAllowsRetry) {
    (void)Extract();
    World.GetComponent<TransformComponent>(Objects[1]).Value.Translation.X = 9;
    const auto frame = Begin();
    NCommon::TaskHandle join;
    bool published = false;
    const auto root = Tasks->Submit([&](NCommon::TaskContext& task) {
        join = Cache.Schedule(World, Resources, Frames->GetFrameStorage(frame), *Tasks, task, [&](const auto&) {
            published = true;
        });
        Tasks->Cancel(join);
    });
    Tasks->Wait(root);
    ASSERT_TRUE(join.IsValid());
    Tasks->Wait(join);
    Tasks->WaitIdle();
    EXPECT_EQ(Tasks->GetStatus(join), NCommon::ETaskStatus::CANCELLED);
    EXPECT_FALSE(published);
    const auto& retry = Extract();
    ASSERT_EQ(retry.GetDelta().UpdatedObjects.size(), 1U);
    EXPECT_FLOAT_EQ((*retry.GetObjects()[1].ResolvedWorldTransform)(0, 3), 9);
}

TEST_P(Incremental, SnapshotOutlivesSourceAndMatchesFlatReference) {
    const auto& snapshot = Extract();
    const auto& reference =
            NRenderer::NInternal::ExtractRenderWorld(World, Resources, Frames->GetFrameStorage(Active.back()));
    ASSERT_EQ(snapshot.GetObjects().size(), reference.GetObjects().size());
    for (std::size_t index = 0; index < snapshot.GetObjects().size(); ++index) {
        EXPECT_EQ(snapshot.GetObjects()[index].Id, reference.GetObjects()[index].Id);
        EXPECT_EQ(snapshot.GetObjects()[index].Mesh.GetVersion(), reference.GetObjects()[index].Mesh.GetVersion());
        EXPECT_EQ(snapshot.GetObjects()[index].ResolvedWorldTransform->GetValues(),
                  NMath::ComposeTransform(reference.GetObjects()[index].WorldTransform).GetValues());
    }
    auto source = std::make_unique<NEcs::World>();
    const auto entity = source->CreateEntity();
    source->AddComponent<TransformComponent>(entity);
    source->AddComponent<RenderableComponent>(entity, RenderableComponent{.Mesh = Mesh, .Material = Material});
    const auto& owned = Extract(source.get());
    source.reset();
    Cache.Reset();
    Resources.RequestUnload(Mesh);
    Resources.CompleteUnload(Mesh);
    Resources.Forget(Mesh);
    ASSERT_EQ(owned.GetObjects().size(), 1U);
    EXPECT_TRUE(owned.GetObjects()[0].Mesh.IsValid());
    EXPECT_NE(owned.GetObjects()[0].Mesh.Get(), nullptr);
}

TEST_P(Incremental, PerspectiveUsesInverseAffineCameraAndRejectsSingularMatrix) {
    World.GetComponent<CameraComponent>(Camera).Projection.VerticalFovRadians = 1;
    auto camera = NMath::Mat4::Identity();
    camera(0, 1) = 0.5F;
    camera(0, 3) = 4;
    World.SetLocalTransform(World.GetEntityReference(Camera), camera);
    const auto& snapshot = Extract();
    const auto& plan =
            NRenderer::Renderer::Prepare(snapshot, Frames->GetFrameStorage(Active.back()).GetMemoryResource());
    const auto& projection = std::as_const(World).GetComponent<CameraComponent>(Camera).Projection;
    const auto combined = plan.GetDrawCommands()[0].ViewProjection * camera;
    const auto expected = NMath::Perspective(projection);
    for (std::size_t index = 0; index < 16; ++index) {
        EXPECT_NEAR(combined.GetValues()[index], expected.GetValues()[index], 0.00001F);
    }
    camera(0, 0) = 0;
    World.SetLocalTransform(World.GetEntityReference(Camera), camera);
    const auto& singular = Extract();
    EXPECT_THROW(
            (void)NRenderer::Renderer::Prepare(singular, Frames->GetFrameStorage(Active.back()).GetMemoryResource()),
            std::exception);
}

INSTANTIATE_TEST_SUITE_P(WorkersAndFrameSlots,
                         Incremental,
                         testing::Values(std::pair{1U, 1U},
                                         std::pair{1U, 2U},
                                         std::pair{1U, 3U},
                                         std::pair{4U, 1U},
                                         std::pair{4U, 2U},
                                         std::pair{4U, 3U}));

} // namespace
