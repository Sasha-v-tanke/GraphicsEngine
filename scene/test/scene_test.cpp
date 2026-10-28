#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include <GraphicsEngine/graphics/material.h>
#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <GraphicsEngine/renderer/render_components.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <GraphicsEngine/scene/scene_manager.h>
#include <engine/controller/frame_scheduler.h>
#include <gtest/gtest.h>
#include <renderer/extraction.h>
#include <tests/common/test_error.h>

namespace NResources {
class MeshData final {};
} // namespace NResources

namespace {

class UserScene final: public NScene::Scene {
public:
    explicit UserScene(std::atomic<int>& destroyed)
        : Scene("user scene")
        , m_destroyed(destroyed) {
    }

    ~UserScene() override {
        ++m_destroyed;
    }

    std::string UserState = "state";

private:
    std::atomic<int>& m_destroyed;
};

TEST(Scene, OwnsWorldAndUserStateWithoutAnotherEcs) {
    std::atomic<int> destroyed{0};
    NScene::SceneManager manager;
    auto scene = std::make_unique<UserScene>(destroyed);
    const auto entity = scene->GetWorld().CreateEntity();
    scene->GetWorld().AddComponent<int>(entity, 42);
    const auto handle = manager.RegisterScene(std::move(scene));
    const auto acquired = manager.Acquire(handle);
    EXPECT_EQ(acquired->GetName(), "user scene");
    EXPECT_EQ(acquired->GetWorld().GetComponent<int>(entity), 42);
    EXPECT_TRUE(manager.IsRegistered(handle));
    EXPECT_FALSE(manager.GetActiveHandle().has_value());
    manager.Activate(handle);
    EXPECT_EQ(manager.GetActiveHandle(), handle);
    EXPECT_EQ(manager.AcquireActive(), acquired);
    manager.Activate(handle);
    EXPECT_EQ(manager.GetActiveHandle(), handle);
    EXPECT_EQ(destroyed.load(), 0);
}

TEST(Scene, RejectsNullStaleAndForeignHandles) {
    NScene::SceneManager manager;
    NScene::SceneManager other;
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { static_cast<void>(manager.RegisterScene(nullptr)); });
    EXPECT_FALSE(manager.IsRegistered({}));
    const auto old = manager.CreateScene();
    manager.Unload(old);
    const auto replacement = manager.CreateScene();
    EXPECT_EQ(old.GetSlotIndex(), replacement.GetSlotIndex());
    EXPECT_NE(old.GetGeneration(), replacement.GetGeneration());
    EXPECT_FALSE(manager.IsRegistered(old));
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { manager.Activate(old); });
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { manager.Unload(old); });
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { static_cast<void>(manager.Acquire(old)); });
    EXPECT_FALSE(other.IsRegistered(replacement));
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { other.Activate(replacement); });
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { other.Unload(replacement); });
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { static_cast<void>(other.Acquire(replacement)); });
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { manager.Activate({}); });
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { manager.Unload({}); });
    EXPECT_TRUE(manager.IsRegistered(replacement));
    EXPECT_FALSE(manager.GetActiveHandle().has_value());
    NTest::ExpectError(NCommon::EError::NOT_FOUND, [&] { static_cast<void>(manager.Acquire({})); });
}

TEST(Scene, SwitchAndUnloadActiveUseExplicitEmptyState) {
    NScene::SceneManager manager;
    const auto first = manager.CreateScene("first");
    const auto second = manager.CreateScene("second");
    manager.Activate(first);
    manager.Activate(second);
    EXPECT_EQ(manager.GetActiveHandle(), second);
    manager.Unload(first);
    EXPECT_EQ(manager.GetActiveHandle(), second);
    manager.Unload(second);
    EXPECT_FALSE(manager.GetActiveHandle().has_value());
    EXPECT_EQ(manager.AcquireActive(), nullptr);
    const auto third = manager.CreateScene("third");
    EXPECT_EQ(manager.AcquireActive(), nullptr);
    manager.Activate(third);
    manager.Deactivate();
    EXPECT_EQ(manager.AcquireActive(), nullptr);
    EXPECT_TRUE(manager.IsRegistered(third));
}

TEST(Scene, AcquiredSceneOutlivesRegistryUnloadAndManager) {
    std::atomic<int> destroyed{0};
    std::shared_ptr<NScene::Scene> lease;
    NScene::SceneHandle old;
    {
        NScene::SceneManager manager;
        old = manager.RegisterScene(std::make_unique<UserScene>(destroyed));
        lease = manager.Acquire(old);
        manager.Activate(old);
        manager.Unload(old);
        EXPECT_EQ(destroyed.load(), 0);
        EXPECT_FALSE(manager.IsRegistered(old));
    }
    EXPECT_EQ(lease->GetName(), "user scene");
    EXPECT_EQ(destroyed.load(), 0);
    NScene::SceneManager replacement;
    const auto current = replacement.CreateScene();
    EXPECT_NE(old, current);
    EXPECT_FALSE(replacement.IsRegistered(old));
    lease.reset();
    EXPECT_EQ(destroyed.load(), 1);
}

TEST(Scene, RegistryOperationsRejectOtherThreads) {
    NScene::SceneManager manager;
    const auto handle = manager.CreateScene();
    manager.Activate(handle);
    auto task = std::async(std::launch::async, [&] {
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(manager.CreateScene()); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE,
                           [&] { static_cast<void>(manager.RegisterScene(std::make_unique<NScene::Scene>())); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(manager.IsRegistered(handle)); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(manager.GetActiveHandle()); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(manager.AcquireActive()); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { manager.Deactivate(); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { manager.Activate(handle); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { manager.Unload(handle); });
        NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(manager.Acquire(handle)); });
    });
    task.get();
    EXPECT_TRUE(manager.IsRegistered(handle));
    EXPECT_EQ(manager.GetActiveHandle(), handle);
}

TEST(Scene, WorkerLeaseRetainsWorldAcrossLogicalUnload) {
    std::atomic<int> destroyed{0};
    NScene::SceneManager manager;
    const auto handle = manager.RegisterScene(std::make_unique<UserScene>(destroyed));
    auto scene = manager.Acquire(handle);
    const auto entity = scene->GetWorld().CreateEntity();
    scene->GetWorld().AddComponent<int>(entity, 73);
    std::promise<void> entered;
    auto started = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    NCommon::TaskSystem tasks{1};
    const auto task = tasks.Submit([scene, entity, &entered, released](NCommon::TaskContext&) {
        entered.set_value();
        EXPECT_EQ(released.wait_for(std::chrono::seconds{2}), std::future_status::ready);
        EXPECT_EQ(scene->GetWorld().GetComponent<int>(entity), 73);
    });
    EXPECT_EQ(started.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    manager.Unload(handle);
    scene.reset();
    EXPECT_EQ(destroyed.load(), 0);
    release.set_value();
    tasks.Wait(task);
    EXPECT_EQ(tasks.GetStatus(task), NCommon::ETaskStatus::COMPLETED);
    EXPECT_EQ(destroyed.load(), 1);
}

TEST(Scene, WorkerLeaseRetainsWorldAfterManagerDestruction) {
    std::atomic<int> destroyed{0};
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    NCommon::TaskSystem tasks{1};
    NCommon::TaskHandle task;
    {
        NScene::SceneManager manager;
        const NScene::SceneHandle handle = manager.RegisterScene(std::make_unique<UserScene>(destroyed));
        std::shared_ptr<NScene::Scene> scene = manager.Acquire(handle);
        const NEcs::Entity entity = scene->GetWorld().CreateEntity();
        scene->GetWorld().AddComponent<int>(entity, 91);
        task = tasks.Submit([scene = std::move(scene), entity, released](NCommon::TaskContext&) {
            ASSERT_EQ(released.wait_for(std::chrono::seconds{5}), std::future_status::ready);
            EXPECT_EQ(scene->GetWorld().GetComponent<int>(entity), 91);
            EXPECT_EQ(scene->GetName(), "user scene");
        });
        // Destroy a still-registered SceneManager while the worker owns the only remaining lease.
    }
    EXPECT_EQ(destroyed.load(), 0);
    release.set_value();
    tasks.Wait(task);
    EXPECT_EQ(tasks.GetStatus(task), NCommon::ETaskStatus::COMPLETED);
    EXPECT_EQ(destroyed.load(), 1);
}

TEST(Scene, ParallelExtractionRetainsOldSceneAcrossSwitchAndUnload) {
    for (const std::size_t workerCount: {1U, 4U}) {
        SCOPED_TRACE(workerCount);
        std::atomic<int> destroyed{0};
        NResources::ResourceManager resources;
        const auto mesh = resources.Request<NResources::MeshData>({"mesh", "parallel scene"});
        resources.PublishReady(resources.BeginLoading(mesh), std::make_shared<NResources::MeshData>());
        const auto material = resources.Request<NGraphics::Material>({"material", "parallel scene"});
        resources.PublishReady(
                resources.BeginLoading(material),
                std::make_shared<NGraphics::Material>(NResources::ResourceIdentity{"pipeline", "shared"}));
        NScene::SceneManager scenes;
        const NScene::SceneHandle old = scenes.RegisterScene(std::make_unique<UserScene>(destroyed));
        const NScene::SceneHandle next = scenes.CreateScene("next");
        scenes.Activate(old);
        std::shared_ptr<NScene::Scene> source = scenes.AcquireActive();
        for (int index = 0; index < 1031; ++index) {
            const NEcs::Entity entity = source->GetWorld().CreateEntity();
            source->GetWorld().AddComponent<NRenderer::TransformComponent>(entity).Value.Translation.X =
                    static_cast<float>(index);
            source->GetWorld().AddComponent<NRenderer::RenderableComponent>(
                    entity,
                    NRenderer::RenderableComponent{.Mesh = mesh, .Material = material});
        }
        NEngine::NController::FrameScheduler scheduler{NEngine::EngineConfig{.MaxActiveFrames = 1}};
        const NEngine::NController::FrameHandle frame = *scheduler.TryAcquireFrame();
        scheduler.ArmFrame(frame);
        scheduler.BeginUpdate(frame);
        scheduler.EndUpdate(frame);
        scheduler.SignalDraw(frame);
        scheduler.BeginFinalize(frame);
        std::promise<void> scheduled;
        std::future<void> started = scheduled.get_future();
        std::promise<void> release;
        const std::shared_future<void> released = release.get_future().share();
        NCommon::TaskSystem tasks{workerCount};
        NCommon::TaskHandle join;
        const NRenderer::RenderWorld* snapshot = nullptr;
        const NCommon::TaskHandle root = tasks.Submit(
                [source = std::move(source), &resources, &scheduler, frame, &join, &snapshot, &scheduled, released](
                        NCommon::TaskContext& task) {
                    join = NRenderer::NInternal::ScheduleRenderWorldExtraction(
                            source->GetWorld(),
                            resources,
                            scheduler.GetFrameStorage(frame),
                            task,
                            [source, &snapshot](const NRenderer::RenderWorld& result) { snapshot = &result; },
                            17);
                    // Chunks depend on this root. Hold them pending until the owner publishes switch/unload.
                    scheduled.set_value();
                    ASSERT_EQ(released.wait_for(std::chrono::seconds{5}), std::future_status::ready);
                });
        EXPECT_EQ(started.wait_for(std::chrono::seconds{5}), std::future_status::ready);
        scenes.Activate(next);
        scenes.Unload(old);
        EXPECT_FALSE(scenes.IsRegistered(old));
        EXPECT_EQ(scenes.GetActiveHandle(), next);
        EXPECT_EQ(destroyed.load(), 0);
        release.set_value();
        tasks.Wait(root);
        ASSERT_EQ(tasks.GetStatus(root), NCommon::ETaskStatus::COMPLETED);
        ASSERT_TRUE(join.IsValid());
        tasks.Wait(join);
        ASSERT_EQ(tasks.GetStatus(join), NCommon::ETaskStatus::COMPLETED);
        EXPECT_EQ(destroyed.load(), 1);
        ASSERT_NE(snapshot, nullptr);
        ASSERT_EQ(snapshot->GetObjects().size(), 1031);
        for (std::size_t index = 0; index < snapshot->GetObjects().size(); ++index) {
            EXPECT_FLOAT_EQ(snapshot->GetObjects()[index].WorldTransform.Translation.X, static_cast<float>(index));
        }
        EXPECT_TRUE(resources.TryAcquire(mesh).has_value());
        scheduler.CompleteFrame(frame);
        scheduler.RecycleFrame(frame);
    }
}

TEST(Scene, UnloadingOldScenePreservesFrameSnapshotAndSharedResources) {
    using namespace NRenderer;
    NResources::ResourceManager resources;
    const auto mesh = resources.Request<NResources::MeshData>({"mesh", "shared"});
    auto meshPayload = std::make_shared<NResources::MeshData>();
    std::weak_ptr<NResources::MeshData> weakMesh = meshPayload;
    resources.PublishReady(resources.BeginLoading(mesh), std::move(meshPayload));
    const auto material = resources.Request<NGraphics::Material>({"material", "shared"});
    resources.PublishReady(resources.BeginLoading(material),
                           std::make_shared<NGraphics::Material>(NResources::ResourceIdentity{"pipeline", "shared"}));
    NScene::SceneManager scenes;
    const auto first = scenes.CreateScene("old");
    const auto second = scenes.CreateScene("new");
    for (const auto handle: {first, second}) {
        auto scene = scenes.Acquire(handle);
        const auto entity = scene->GetWorld().CreateEntity();
        scene->GetWorld().AddComponent<TransformComponent>(entity).Value.Translation.X = handle == first ? 1.0F : 2.0F;
        scene->GetWorld().AddComponent<RenderableComponent>(entity,
                                                            RenderableComponent{.Mesh = mesh, .Material = material});
    }
    scenes.Activate(first);
    NEngine::NController::FrameScheduler scheduler{NEngine::EngineConfig{.MaxActiveFrames = 2}};
    const auto extract = [&] {
        const auto frame = *scheduler.TryAcquireFrame();
        scheduler.ArmFrame(frame);
        scheduler.BeginUpdate(frame);
        scheduler.EndUpdate(frame);
        scheduler.SignalDraw(frame);
        scheduler.BeginFinalize(frame);
        auto scene = scenes.AcquireActive();
        return std::pair{
                frame,
                &NInternal::ExtractRenderWorld(scene->GetWorld(), resources, scheduler.GetFrameStorage(frame))};
    };
    const auto [oldFrame, oldSnapshot] = extract();
    std::promise<void> gpuDone;
    scheduler.MarkSubmitted(oldFrame, gpuDone.get_future().share());
    scheduler.BeginGpu(oldFrame);
    scenes.Activate(second);
    scenes.Unload(first);
    const auto [newFrame, newSnapshot] = extract();
    ASSERT_EQ(oldSnapshot->GetObjects().size(), 1);
    ASSERT_EQ(newSnapshot->GetObjects().size(), 1);
    EXPECT_EQ(oldSnapshot->GetObjects()[0].WorldTransform.Translation.X, 1.0F);
    EXPECT_EQ(newSnapshot->GetObjects()[0].WorldTransform.Translation.X, 2.0F);
    EXPECT_EQ(oldSnapshot->GetObjects()[0].Mesh.GetResource(), newSnapshot->GetObjects()[0].Mesh.GetResource());
    EXPECT_TRUE(resources.TryAcquire(mesh).has_value());
    EXPECT_EQ(scheduler.GetState(oldFrame), NEngine::NController::EFrameState::GPU_IN_FLIGHT);
    scenes.Unload(second);
    EXPECT_TRUE(resources.TryAcquire(mesh).has_value());
    resources.RequestUnload(mesh);
    resources.CompleteUnload(mesh);
    EXPECT_FALSE(weakMesh.expired());
    gpuDone.set_value();
    scheduler.CompleteGpu(oldFrame);
    scheduler.RecycleFrame(oldFrame);
    EXPECT_FALSE(weakMesh.expired());
    scheduler.AbortFrame(newFrame);
    EXPECT_TRUE(weakMesh.expired());
}

} // namespace
