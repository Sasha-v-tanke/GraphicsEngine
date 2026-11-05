#include <cmath>
#include <limits>
#include <memory_resource>

#include <engine/controller/frame_scheduler.h>
#include <gtest/gtest.h>
#include <renderer/extraction.h>
#include <samples/sample_7_model/scenario.h>
#include <tests/common/test_error.h>

namespace {
NEngine::NController::FrameHandle BeginFrame(NEngine::NController::FrameScheduler& scheduler) {
    const auto frame = *scheduler.TryAcquireFrame();
    scheduler.ArmFrame(frame);
    scheduler.BeginUpdate(frame);
    scheduler.EndUpdate(frame);
    scheduler.SignalDraw(frame);
    scheduler.BeginFinalize(frame);
    return frame;
}

TEST(FirstModel, LoadsObjTextureAndPublishesPerspectiveSnapshotBeforeWorldChanges) {
    NModel::Scene scene;
    const auto mesh = scene.Resources.TryAcquire(scene.Mesh);
    ASSERT_TRUE(mesh);
    EXPECT_EQ((*mesh)->GetVertices().size(), 24U);
    EXPECT_EQ((*mesh)->GetIndices().size(), 36U);
    for (std::uint8_t mask: (*mesh)->GetAttributeMasks()) {
        EXPECT_EQ(mask, NResources::MeshData::NORMAL_PRESENT | NResources::MeshData::TEXCOORD_PRESENT);
    }
    const auto image = scene.Resources.TryAcquire(scene.Image);
    ASSERT_TRUE(image);
    EXPECT_EQ((*image)->GetWidth(), 16U);
    EXPECT_EQ((*image)->GetHeight(), 16U);

    scene.SetViewport(640, 400);
    scene.Update(17);
    const NMath::Transform model = scene.World.GetComponent<NRenderer::TransformComponent>(scene.Model).Value;
    NEngine::NController::FrameScheduler scheduler{{.MaxActiveFrames = 1}};
    const auto frame = BeginFrame(scheduler);
    const auto storage = scheduler.GetFrameStorage(frame);
    const auto& snapshot = NRenderer::NInternal::ExtractRenderWorld(scene.World, scene.Resources, storage);
    // Preparation must depend exclusively on the snapshot, including camera values and pinned mesh/material.
    scene.Update(89);
    scene.World.DestroyEntity(scene.Camera);
    scene.World.DestroyEntity(scene.Model);
    scene.Resources.RequestUnload(scene.Mesh);
    scene.Resources.CompleteUnload(scene.Mesh);
    const auto& plan = NRenderer::Renderer::Prepare(snapshot, storage.GetMemoryResource());
    ASSERT_EQ(plan.GetDrawCommands().size(), 1U);
    const auto& command = plan.GetDrawCommands()[0];
    EXPECT_FLOAT_EQ(command.WorldTransform.Rotation.Y, model.Rotation.Y);
    const auto expected =
            NMath::Perspective({1.04719755F, 1.6F, 0.1F, 100.0F}) * NMath::LookAt({0, 0, 4}, {0, 0, 0}, {0, 1, 0});
    for (std::size_t index = 0; index < 16; ++index) {
        EXPECT_NEAR(command.ViewProjection.GetValues()[index], expected.GetValues()[index], 0.00001F);
    }
    const NMath::Vec4 clip =
            command.ViewProjection * NMath::ComposeTransform(command.WorldTransform) * NMath::Vec4{0, 0, 0, 1};
    EXPECT_GT(clip.W, 0.0F);
    EXPECT_GT(clip.Z / clip.W, 0.0F);
    EXPECT_LT(clip.Z / clip.W, 1.0F);
}

TEST(FirstModel, CameraInverseIncludesRotationAndScale) {
    NModel::Scene scene;
    auto& camera = scene.World.GetComponent<NRenderer::TransformComponent>(scene.Camera).Value;
    camera.Rotation = NMath::AngleAxis(0.7F, {0, 1, 0});
    camera.Scale = {2, 3, 4};
    const auto projection = scene.World.GetComponent<NRenderer::CameraComponent>(scene.Camera).Projection;
    NEngine::NController::FrameScheduler scheduler{{.MaxActiveFrames = 1}};
    const auto frame = BeginFrame(scheduler);
    const auto storage = scheduler.GetFrameStorage(frame);
    const auto& snapshot = NRenderer::NInternal::ExtractRenderWorld(scene.World, scene.Resources, storage);
    const auto& plan = NRenderer::Renderer::Prepare(snapshot, storage.GetMemoryResource());
    const auto actual = plan.GetDrawCommands()[0].ViewProjection * NMath::ComposeTransform(camera);
    const auto expected = NMath::Perspective(projection);
    for (std::size_t index = 0; index < 16; ++index) {
        EXPECT_NEAR(actual.GetValues()[index], expected.GetValues()[index], 0.00001F);
    }
}

TEST(FirstModel, RejectsInvalidCameraAndMissingAssets) {
    EXPECT_THROW(NModel::Scene{"/graphicsengine-missing-model-assets"}, std::runtime_error);
    NModel::Scene scene;
    scene.World.GetComponent<NRenderer::CameraComponent>(scene.Camera).Projection.AspectRatio =
            std::numeric_limits<float>::quiet_NaN();
    NEngine::NController::FrameScheduler scheduler{{.MaxActiveFrames = 1}};
    const auto frame = BeginFrame(scheduler);
    const auto storage = scheduler.GetFrameStorage(frame);
    const auto& snapshot = NRenderer::NInternal::ExtractRenderWorld(scene.World, scene.Resources, storage);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { (void)NRenderer::Renderer::Prepare(snapshot, storage.GetMemoryResource()); });
}
} // namespace
