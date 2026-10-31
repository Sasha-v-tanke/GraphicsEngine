#include <stdexcept>

#include <graphics/material.h>
#include <gtest/gtest.h>
#include <renderer/execution_validation.h>
#include <resources/mesh_data.h>
#include <resources/resource_manager.h>
#include <tests/common/test_error.h>

namespace {
struct GraphExecutionTest: testing::Test {
    NResources::ResourceManager Resources;
    NRenderer::RenderFrameIdentity Frame{.Generation = 1};
    std::shared_ptr<const NRenderer::SwapchainGraphTarget> Target = std::make_shared<NRenderer::SwapchainGraphTarget>(
            1,
            NRenderer::GraphImageDescription{{64, 64, 1},
                                             NGraphics::EPixelFormat::BGRA8_SRGB,
                                             NGraphics::ESampleCount::X1,
                                             NGraphics::ImageUsage(NGraphics::EImageUsage::ColorAttachment)});
    NGraphics::GraphicsPipelineDescriptor Pipeline;
    NRenderer::DrawCommandData Draw;
    GraphExecutionTest() {
        Draw.Mesh = Resources.Request<NResources::MeshData>({"mesh", "one"});
        Draw.Material = Resources.Request<NGraphics::Material>({"material", "one"});
        Draw.MeshVersion = Draw.MaterialVersion = 1;
        Pipeline.ColorAttachmentFormats = {NGraphics::EPixelFormat::BGRA8_SRGB};
    }
    NRenderer::CompiledGraph Build(const NRenderer::DrawCommandData& draw) const {
        NRenderer::RenderGraphBuilder builder{Frame};
        const auto color = builder.ImportSwapchain(Target, NRenderer::EResourceUsage::UNDEFINED);
        NRenderer::GraphicsPassDescription pass;
        pass.Writes = {{color, NRenderer::EResourceUsage::COLOR_ATTACHMENT_WRITE}};
        pass.ColorAttachments = {{.Resource = color}};
        pass.DrawCommands = {draw};
        (void)builder.AddGraphicsPass(pass);
        pass.ColorAttachments[0].Load = NRenderer::EAttachmentLoad::LOAD;
        (void)builder.AddGraphicsPass(pass);
        return builder.Compile();
    }
    void Validate(const NRenderer::CompiledGraph& graph) {
        NRenderer::NInternal::ValidateColorExecution(graph, Frame, Target, [&](const auto&) -> const auto& {
            return Pipeline;
        });
    }
};
TEST_F(GraphExecutionTest, ValidatesDrawsInCompiledPassOrder) {
    int calls = 0;
    NRenderer::NInternal::ValidateColorExecution(Build(Draw), Frame, Target, [&](const auto& draw) -> const auto& {
        EXPECT_EQ(draw.Mesh, Draw.Mesh);
        ++calls;
        return Pipeline;
    });
    EXPECT_EQ(calls, 2);
}
TEST_F(GraphExecutionTest, RejectsMalformedDrawAndIncompatiblePipelineBeforeRecording) {
    auto malformed = Draw;
    malformed.MeshVersion = 0;
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { Validate(Build(malformed)); });
    Pipeline.ColorAttachmentFormats = {NGraphics::EPixelFormat::RGBA8_UNORM};
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { Validate(Build(Draw)); });
}
TEST_F(GraphExecutionTest, RejectsForeignFrameAndMissingPreparedVersions) {
    const auto graph = Build(Draw);
    auto other = Frame;
    ++other.Generation;
    EXPECT_ANY_THROW(
            NRenderer::NInternal::ValidateColorExecution(graph, other, Target, [&](const auto&) -> const auto& {
                return Pipeline;
            }));
    EXPECT_THROW(NRenderer::NInternal::ValidateColorExecution(
                         graph,
                         Frame,
                         Target,
                         [&](const auto&) -> const NGraphics::GraphicsPipelineDescriptor& {
                             throw std::runtime_error{"stale version"};
                         }),
                 std::runtime_error);
}
} // namespace
