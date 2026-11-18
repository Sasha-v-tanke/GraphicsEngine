#include <algorithm>
#include <memory>
#include <vector>

#include <GraphicsEngine/graphics/material.h>
#include <GraphicsEngine/renderer/standard_graph.h>
#include <GraphicsEngine/resources/mesh_data.h>
#include <engine/controller/frame_scheduler.h>
#include <gtest/gtest.h>
#include <renderer/extraction.h>
#include <tests/common/test_error.h>

namespace {

class StandardGraph: public testing::Test {
protected:
    NResources::ResourceManager Resources;
    NEcs::World World;
    std::vector<NEcs::Entity> Entities;
    NEngine::NController::FrameScheduler Frames{{.MaxActiveFrames = 1}};
    std::shared_ptr<NRenderer::SwapchainGraphTarget> Target = MakeTarget(1, 64);

    static std::shared_ptr<NRenderer::SwapchainGraphTarget> MakeTarget(std::uint64_t id, std::uint32_t width) {
        return std::make_shared<NRenderer::SwapchainGraphTarget>(
                id,
                NRenderer::GraphImageDescription{{width, 32, 1},
                                                 NGraphics::EPixelFormat::BGRA8_SRGB,
                                                 NGraphics::ESampleCount::X1,
                                                 NGraphics::ImageUsage(NGraphics::EImageUsage::ColorAttachment)});
    }

    void SetUp() override {
        const auto mesh = Resources.Request<NResources::MeshData>({"mesh", "triangle"});
        Resources.PublishReady(Resources.BeginLoading(mesh),
                               std::make_shared<NResources::MeshData>(std::vector<NResources::MeshVertex>(3),
                                                                      std::vector<std::uint32_t>{0, 1, 2},
                                                                      std::vector<std::uint8_t>(3)));
        const auto material = Resources.Request<NGraphics::Material>({"material", "opaque"});
        Resources.PublishReady(
                Resources.BeginLoading(material),
                std::make_shared<NGraphics::Material>(NResources::ResourceIdentity{"pipeline", "opaque"}));
        for (int index = 0; index < 2; ++index) {
            const auto camera = World.CreateEntity();
            Entities.push_back(camera);
            World.AddComponent<NRenderer::TransformComponent>(
                    camera,
                    NRenderer::TransformComponent{.Value = {.Translation = {0, 0, static_cast<float>(index + 2)}}});
            World.AddComponent<NRenderer::CameraComponent>(
                    camera,
                    NRenderer::CameraComponent{.Projection = {1.0F, 2.0F, 0.1F, 100.0F}});
        }
        for (int index = 0; index < 3; ++index) {
            const auto object = World.CreateEntity();
            Entities.push_back(object);
            World.AddComponent<NRenderer::TransformComponent>(object);
            World.AddComponent<NRenderer::RenderableComponent>(
                    object,
                    NRenderer::RenderableComponent{.Mesh = mesh, .Material = material});
        }
    }

    const NRenderer::RenderPlan& Prepare() {
        const auto frame = *Frames.TryAcquireFrame();
        Frames.ArmFrame(frame);
        Frames.BeginUpdate(frame);
        Frames.EndUpdate(frame);
        Frames.SignalDraw(frame);
        Frames.BeginFinalize(frame);
        const auto storage = Frames.GetFrameStorage(frame);
        const auto& world = NRenderer::NInternal::ExtractRenderWorld(World, Resources, storage);
        return NRenderer::Renderer::Prepare(world, storage.GetMemoryResource());
    }
};

TEST_F(StandardGraph, FiltersSelectedViewPreservingEveryDrawAndCameraMatrix) {
    const auto& plan = Prepare();
    ASSERT_EQ(plan.GetDrawCommands().size(), 6U);
    const auto camera = plan.GetDrawCommands().back().ViewId;
    const auto graph = NRenderer::BuildStandardGraph(plan.GetFrame(), plan, camera, Target);
    ASSERT_EQ(graph.GetPasses().size(), 1U);
    const auto& pass = graph.GetPasses()[0].Description;
    ASSERT_EQ(pass.DrawCommands.size(), 3U);
    std::size_t index = 0;
    for (const auto& expected: plan.GetDrawCommands()) {
        if (expected.ViewId != camera) {
            continue;
        }
        const auto& actual = pass.DrawCommands[index++];
        EXPECT_EQ(actual.ViewId, camera);
        EXPECT_EQ(actual.ObjectId, expected.ObjectId);
        EXPECT_EQ(actual.Mesh, expected.Mesh);
        EXPECT_EQ(actual.Material, expected.Material);
        EXPECT_EQ(actual.MeshVersion, expected.MeshVersion);
        EXPECT_EQ(actual.MaterialVersion, expected.MaterialVersion);
        EXPECT_EQ(actual.ViewProjection.GetValues(), expected.ViewProjection.GetValues());
    }
    ASSERT_TRUE(pass.DepthAttachment);
    const auto& depth = graph.GetResource(pass.DepthAttachment->Resource);
    EXPECT_EQ(depth.Lifetime, NRenderer::EGraphResourceLifetime::TRANSIENT);
    EXPECT_EQ(depth.ImageDescription.Format, NGraphics::EPixelFormat::D32_FLOAT);
    EXPECT_EQ(depth.ImageDescription.Usage, NGraphics::ImageUsage(NGraphics::EImageUsage::DepthStencilAttachment));
    EXPECT_EQ(depth.ImageDescription.Extent.Width, 64U);
    EXPECT_EQ(depth.ImageDescription.Extent.Height, 32U);
    EXPECT_EQ(depth.InitialUsage, NRenderer::EResourceUsage::UNDEFINED);
    EXPECT_EQ(depth.FinalUsage, NRenderer::EResourceUsage::UNDEFINED);
    EXPECT_EQ(pass.DepthAttachment->Load, NRenderer::EAttachmentLoad::CLEAR);
    EXPECT_EQ(pass.DepthAttachment->Store, NRenderer::EAttachmentStore::DISCARD);
    EXPECT_FLOAT_EQ(pass.DepthAttachment->ClearDepth, 1.0F);
    const auto lifetime =
            std::ranges::find(graph.GetLifetimes(), depth.Id, &NRenderer::GraphResourceLifetime::Resource);
    ASSERT_NE(lifetime, graph.GetLifetimes().end());
    EXPECT_EQ(lifetime->FirstPass, 0U);
    EXPECT_EQ(lifetime->LastPass, 0U);
    EXPECT_TRUE(lifetime->Allocation);
    EXPECT_EQ(graph.GetTransitions().back().After, NRenderer::EResourceUsage::PRESENT);
    EXPECT_NO_THROW(graph.ValidateResources(plan.GetFrame()));
}

TEST_F(StandardGraph, NoCameraUnknownViewAndMissingResourcesClearWithoutDepthOrBuffers) {
    const auto& plan = Prepare();
    for (const auto camera: {std::optional<NRenderer::RenderObjectId>{},
                             std::optional<NRenderer::RenderObjectId>{{987, 2}},
                             std::optional<NRenderer::RenderObjectId>{{plan.GetDrawCommands()[0].ViewId.Index, 99}}}) {
        const auto graph = NRenderer::BuildStandardGraph(plan.GetFrame(), plan, camera, Target);
        ASSERT_EQ(graph.GetResources().size(), 1U);
        ASSERT_EQ(graph.GetPasses().size(), 1U);
        const auto& pass = graph.GetPasses()[0].Description;
        EXPECT_TRUE(pass.DrawCommands.empty());
        EXPECT_FALSE(pass.DepthAttachment);
        EXPECT_TRUE(pass.Reads.empty());
        EXPECT_EQ(pass.ColorAttachments[0].Load, NRenderer::EAttachmentLoad::CLEAR);
        EXPECT_EQ(pass.ColorAttachments[0].Store, NRenderer::EAttachmentStore::STORE);
    }
}

TEST_F(StandardGraph, LoadingResourcesProduceClearOnly) {
    for (std::size_t index = 2; index < Entities.size(); ++index) {
        World.GetComponent<NRenderer::RenderableComponent>(Entities[index]).Mesh =
                Resources.Request<NResources::MeshData>({"mesh", "loading"});
    }
    const auto& plan = Prepare();
    EXPECT_TRUE(plan.GetDrawCommands().empty());
    const auto graph = NRenderer::BuildStandardGraph(plan.GetFrame(), plan, NRenderer::RenderObjectId{0, 0}, Target);
    EXPECT_EQ(graph.GetResources().size(), 1U);
    EXPECT_TRUE(graph.GetPasses()[0].Description.DrawCommands.empty());
}

TEST_F(StandardGraph, EmptyWorldHasValidClearPresentGraph) {
    for (const auto entity: Entities) {
        World.DestroyEntity(entity);
    }
    const auto& plan = Prepare();
    const auto graph = NRenderer::BuildStandardGraph(plan.GetFrame(), plan, std::nullopt, Target);
    EXPECT_EQ(graph.GetResources().size(), 1U);
    EXPECT_TRUE(graph.GetPasses()[0].Description.DrawCommands.empty());
    EXPECT_EQ(graph.GetTransitions().back().After, NRenderer::EResourceUsage::PRESENT);
}

TEST_F(StandardGraph, RejectsWrongFrameInvalidCameraAndStaleTargets) {
    const auto& plan = Prepare();
    auto frame = plan.GetFrame();
    ++frame.Generation;
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { (void)NRenderer::BuildStandardGraph(frame, plan, std::nullopt, Target); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)NRenderer::BuildStandardGraph(plan.GetFrame(), plan, NRenderer::RenderObjectId{}, Target);
    });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { (void)NRenderer::BuildStandardGraph(plan.GetFrame(), plan, std::nullopt, nullptr); });
    const auto graph = NRenderer::BuildStandardGraph(plan.GetFrame(), plan, std::nullopt, Target);
    Target->Invalidate();
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { graph.ValidateResources(plan.GetFrame()); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { (void)NRenderer::BuildStandardGraph(plan.GetFrame(), plan, std::nullopt, Target); });
    Target = MakeTarget(2, 128);
    const auto resized = NRenderer::BuildStandardGraph(plan.GetFrame(), plan, plan.GetDrawCommands()[0].ViewId, Target);
    EXPECT_EQ(resized.GetResources()[0].ImageDescription.Extent.Width, 128U);
    EXPECT_NE(graph.GetResources()[0].Id, resized.GetResources()[0].Id);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { (void)resized.GetResource(graph.GetResources()[0].Id); });
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { resized.ValidateResources(frame); });
    Target.reset();
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { resized.ValidateResources(plan.GetFrame()); });
}

TEST_F(StandardGraph, RepeatableDeclarationsAndScopedExtensions) {
    const auto& plan = Prepare();
    const auto camera = plan.GetDrawCommands()[0].ViewId;
    NRenderer::StandardGraphOptions options;
    options.ClearColor = {0.1F, 0.2F, 0.3F, 1.0F};
    options.InitialColorUsage = NRenderer::EResourceUsage::PRESENT;
    options.Extend = [](NRenderer::RenderGraphBuilder& builder, const NRenderer::StandardGraphOutputs& output) {
        NRenderer::GraphicsPassDescription overlay;
        overlay.Writes.push_back({output.Color, NRenderer::EResourceUsage::COLOR_ATTACHMENT_WRITE});
        overlay.ColorAttachments.push_back({.Resource = output.Color, .Load = NRenderer::EAttachmentLoad::LOAD});
        const auto pass = builder.AddGraphicsPass(overlay);
        builder.AddDependency(output.OpaquePass, pass);
    };
    const auto one = NRenderer::BuildStandardGraph(plan.GetFrame(), plan, camera, Target, options);
    const auto two = NRenderer::BuildStandardGraph(plan.GetFrame(), plan, camera, Target, options);
    ASSERT_EQ(one.GetPasses().size(), 2U);
    ASSERT_EQ(two.GetPasses().size(), 2U);
    EXPECT_EQ(one.GetTransitions().size(), two.GetTransitions().size());
    EXPECT_EQ(one.GetRecordingRanges().size(), two.GetRecordingRanges().size());
    EXPECT_EQ(one.GetDependencies().size(), two.GetDependencies().size());
    for (std::size_t index = 0; index < one.GetTransitions().size(); ++index) {
        EXPECT_EQ(one.GetTransitions()[index].Before, two.GetTransitions()[index].Before);
        EXPECT_EQ(one.GetTransitions()[index].After, two.GetTransitions()[index].After);
        EXPECT_EQ(one.GetTransitions()[index].PassIndex, two.GetTransitions()[index].PassIndex);
    }
    EXPECT_EQ(one.GetPasses()[0].Description.ColorAttachments[0].ClearColor, options.ClearColor);
    options.Extend = [](NRenderer::RenderGraphBuilder&, const NRenderer::StandardGraphOutputs&) { throw 42; };
    EXPECT_THROW((void)NRenderer::BuildStandardGraph(plan.GetFrame(), plan, camera, Target, options), int);
    EXPECT_NO_THROW((void)NRenderer::BuildStandardGraph(plan.GetFrame(), plan, camera, Target));
}

} // namespace
