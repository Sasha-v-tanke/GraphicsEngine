#include <future>
#include <utility>

#include <application/view_registry.h>
#include <engine/controller/frame_scheduler.h>
#include <gtest/gtest.h>
#include <renderer/extraction.h>
#include <tests/common/test_error.h>

namespace {

class ViewRegistryTest: public testing::Test {
protected:
    NScene::SceneManager Scenes;
    NApplication::ViewRegistry Views;
    NResources::ResourceManager Resources;
    NEngine::NController::FrameScheduler Scheduler{NEngine::EngineConfig{.MaxActiveFrames = 3}};
    NScene::SceneHandle Scene = Scenes.CreateScene();
    NEcs::EntityReference Camera = AddCamera(Scene, 7);

    NEcs::EntityReference AddCamera(NScene::SceneHandle scene, float x) {
        auto& world = Scenes.Acquire(scene)->GetWorld();
        auto entity = world.CreateEntity();
        world.AddComponent<NRenderer::TransformComponent>(entity).Value.Translation.X = x;
        world.AddComponent<NRenderer::CameraComponent>(entity).Projection.AspectRatio = 9;
        return world.GetEntityReference(entity);
    }
    void SetUp() override {
        Scenes.Activate(Scene);
        Views.SetFramebufferSize(Views.GetMainView(), 800, 400);
        Views.BindCamera(Views.GetMainView(), Scenes, Scene, Camera);
    }
    const NRenderer::RenderWorld& Extract(NScene::SceneHandle scene) {
        auto frame = *Scheduler.TryAcquireFrame();
        Scheduler.ArmFrame(frame);
        Scheduler.BeginUpdate(frame);
        Scheduler.EndUpdate(frame);
        Scheduler.SignalDraw(frame);
        Scheduler.BeginFinalize(frame);
        return NRenderer::NInternal::ExtractRenderWorld(Scenes.Acquire(scene)->GetWorld(),
                                                        Resources,
                                                        Scheduler.GetFrameStorage(frame));
    }
};

TEST_F(ViewRegistryTest, SelectsExplicitCameraAndCopiesFrameDataWithoutMutatingEcs) {
    auto chosen = AddCamera(Scene, 42);
    Views.BindCamera(Views.GetMainView(), Scenes, Scene, chosen);
    const auto& world = Extract(Scene);
    const auto publication = Views.Publish(Views.GetMainView(), Scenes, world);
    ASSERT_TRUE(publication.GetCamera());
    EXPECT_EQ(publication.GetDiagnostic(), NApplication::EViewDiagnostic::NONE);
    EXPECT_EQ(publication.GetFrame(), world.GetFrame());
    EXPECT_EQ(publication.GetCameraIdentity(), chosen);
    EXPECT_EQ(publication.GetScene(), Scene);
    EXPECT_FLOAT_EQ(publication.GetCamera()->WorldTransform.Translation.X, 42);
    EXPECT_FLOAT_EQ(publication.GetCamera()->Projection.AspectRatio, 2);
    EXPECT_FLOAT_EQ(Scenes.Acquire(Scene)
                            ->GetWorld()
                            .GetComponent<NRenderer::CameraComponent>(chosen.Identity)
                            .Projection.AspectRatio,
                    9);

    Views.UnbindCamera(Views.GetMainView());
    EXPECT_EQ(Views.Publish(Views.GetMainView(), Scenes, world).GetDiagnostic(),
              NApplication::EViewDiagnostic::UNBOUND);
    Views.SetFramebufferSize(Views.GetMainView(), 300, 600);
    Views.BindCamera(Views.GetMainView(), Scenes, Scene, Camera);
    auto next = Views.Publish(Views.GetMainView(), Scenes, world);
    EXPECT_FLOAT_EQ(next.GetCamera()->Projection.AspectRatio, 0.5F);
    EXPECT_FLOAT_EQ(publication.GetCamera()->Projection.AspectRatio, 2);
    EXPECT_EQ(publication.GetWidth(), 800U);
    EXPECT_EQ(publication.GetHeight(), 400U);
    Scenes.Unload(Scene);
    EXPECT_FLOAT_EQ(publication.GetCamera()->WorldTransform.Translation.X, 42);
}

TEST_F(ViewRegistryTest, RejectsForeignStaleAndRestartedViewHandles) {
    NApplication::ViewRegistry other;
    EXPECT_FALSE(other.IsRegistered(Views.GetMainView()));
    EXPECT_THROW(Views.UnbindCamera(other.GetMainView()), NCommon::Exception);
    EXPECT_THROW(Views.DestroyView(Views.GetMainView()), NCommon::Exception);
    EXPECT_FALSE(Views.IsRegistered({}));
    const auto retired = Views.CreateView();
    Views.DestroyView(retired);
    const auto reused = Views.CreateView();
    EXPECT_NE(retired, reused);
    EXPECT_FALSE(Views.IsRegistered(retired));
    EXPECT_THROW(Views.SetFramebufferSize(retired, 1, 1), NCommon::Exception);
    EXPECT_TRUE(Views.IsRegistered(reused));
    NApplication::ViewHandle old;
    {
        NApplication::ViewRegistry prior;
        old = prior.GetMainView();
    }
    NApplication::ViewRegistry restarted;
    EXPECT_FALSE(restarted.IsRegistered(old));
}

TEST_F(ViewRegistryTest, RejectsForeignSceneAndEntityAndRecycledCameraGeneration) {
    auto second = Scenes.CreateScene();
    const auto other = AddCamera(second, 3);
    EXPECT_THROW(Views.BindCamera(Views.GetMainView(), Scenes, Scene, other), NCommon::Exception);
    NScene::SceneManager foreign;
    auto foreignScene = foreign.CreateScene();
    EXPECT_THROW(Views.BindCamera(Views.GetMainView(), Scenes, foreignScene, Camera), NCommon::Exception);
    auto& ecs = Scenes.Acquire(Scene)->GetWorld();
    ecs.DestroyEntity(Camera.Identity);
    auto replacement = AddCamera(Scene, 55);
    EXPECT_EQ(replacement.Identity.Index, Camera.Identity.Index);
    EXPECT_NE(replacement.Identity.Generation, Camera.Identity.Generation);
    EXPECT_THROW(Views.BindCamera(Views.GetMainView(), Scenes, Scene, Camera), NCommon::Exception);
    const auto result = Views.Publish(Views.GetMainView(), Scenes, Extract(Scene));
    EXPECT_FALSE(result.GetCamera());
    EXPECT_EQ(result.GetDiagnostic(), NApplication::EViewDiagnostic::CAMERA_UNAVAILABLE);
}

TEST_F(ViewRegistryTest, SceneSwitchUnloadAndNoActiveSceneProduceClearOnlyDiagnostics) {
    const auto& firstWorld = Extract(Scene);
    Scenes.Deactivate();
    EXPECT_EQ(Views.Publish(Views.GetMainView(), Scenes, firstWorld).GetDiagnostic(),
              NApplication::EViewDiagnostic::NO_ACTIVE_SCENE);
    auto second = Scenes.CreateScene();
    auto camera = AddCamera(second, 3);
    Scenes.Activate(second);
    EXPECT_EQ(Views.Publish(Views.GetMainView(), Scenes, firstWorld).GetDiagnostic(),
              NApplication::EViewDiagnostic::INACTIVE_SCENE);
    Views.BindCamera(Views.GetMainView(), Scenes, second, camera);
    auto mismatch = Views.Publish(Views.GetMainView(), Scenes, firstWorld);
    EXPECT_FALSE(mismatch.GetCamera());
    EXPECT_EQ(mismatch.GetDiagnostic(), NApplication::EViewDiagnostic::SOURCE_MISMATCH);
    const auto& secondWorld = Extract(second);
    const auto published = Views.Publish(Views.GetMainView(), Scenes, secondWorld);
    ASSERT_TRUE(published.GetCamera());
    Scenes.Unload(second);
    auto replacement = Scenes.CreateScene();
    Scenes.Activate(replacement);
    EXPECT_EQ(Views.Publish(Views.GetMainView(), Scenes, secondWorld).GetDiagnostic(),
              NApplication::EViewDiagnostic::STALE_SCENE);
    EXPECT_TRUE(published.GetCamera());
}

TEST_F(ViewRegistryTest, IndependentViewsResizeAndZeroExtent) {
    auto second = Views.CreateView();
    auto secondCamera = AddCamera(Scene, 10);
    Views.BindCamera(second, Scenes, Scene, secondCamera);
    Views.SetFramebufferSize(second, 100, 300);
    const auto& world = Extract(Scene);
    auto first = Views.Publish(Views.GetMainView(), Scenes, world);
    auto other = Views.Publish(second, Scenes, world);
    EXPECT_FLOAT_EQ(first.GetCamera()->WorldTransform.Translation.X, 7);
    EXPECT_FLOAT_EQ(other.GetCamera()->WorldTransform.Translation.X, 10);
    EXPECT_FLOAT_EQ(other.GetCamera()->Projection.AspectRatio, 1.0F / 3.0F);
    for (auto [width, height]: {std::pair{0U, 100U}, std::pair{100U, 0U}, std::pair{0U, 0U}}) {
        Views.SetFramebufferSize(second, width, height);
        auto suspended = Views.Publish(second, Scenes, world);
        EXPECT_FALSE(suspended.GetCamera());
        EXPECT_EQ(suspended.GetDiagnostic(), NApplication::EViewDiagnostic::ZERO_EXTENT);
    }
    EXPECT_TRUE(Views.Publish(Views.GetMainView(), Scenes, world).GetCamera());
}

TEST_F(ViewRegistryTest, DisabledEntityAndComponentsAndMissingCameraAreUnavailable) {
    auto& ecs = Scenes.Acquire(Scene)->GetWorld();
    const auto& world = Extract(Scene);
    const auto expectUnavailable = [&] {
        auto result = Views.Publish(Views.GetMainView(), Scenes, world);
        EXPECT_FALSE(result.GetCamera());
        EXPECT_EQ(result.GetDiagnostic(), NApplication::EViewDiagnostic::CAMERA_UNAVAILABLE);
    };
    ecs.QueueSetEnabled(Camera, false);
    ecs.CommitPending();
    expectUnavailable();
    ecs.QueueSetEnabled(Camera, true);
    ecs.QueueSetComponentEnabled<NRenderer::CameraComponent>(Camera, false);
    ecs.CommitPending();
    expectUnavailable();
    ecs.QueueSetComponentEnabled<NRenderer::CameraComponent>(Camera, true);
    ecs.QueueSetComponentEnabled<NRenderer::TransformComponent>(Camera, false);
    ecs.CommitPending();
    expectUnavailable();
    ecs.QueueRemoveComponent<NRenderer::CameraComponent>(Camera);
    ecs.CommitPending();
    expectUnavailable();
}

TEST_F(ViewRegistryTest, RejectsCallsFromNonOwnerThread) {
    const auto main = Views.GetMainView();
    auto result = std::async(std::launch::async, [&] {
        EXPECT_THROW((void)Views.IsRegistered(main), NCommon::Exception);
        EXPECT_THROW((void)Views.CreateView(), NCommon::Exception);
    });
    result.get();
}

} // namespace
