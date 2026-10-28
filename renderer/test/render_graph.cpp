#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include <GraphicsEngine/renderer/render_graph.h>
#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {

using NRenderer::EAttachmentLoad;
using NRenderer::EAttachmentStore;
using NRenderer::EResourceUsage;
using NRenderer::GraphicsPassDescription;
using NRenderer::GraphResourceId;
using NRenderer::RenderGraphBuilder;
using NRenderer::SwapchainGraphTarget;
using NTest::ExpectError;

constexpr NRenderer::RenderFrameIdentity FRAME{.ApplicationFrameIndex = 7, .FrameSlotIndex = 1, .Generation = 3};

NRenderer::GraphImageDescription TargetDescription(std::uint32_t width = 64) {
    return {{width, 64, 1},
            NGraphics::EPixelFormat::BGRA8_SRGB,
            NGraphics::ESampleCount::X1,
            NGraphics::EImageUsage::ColorAttachment | NGraphics::EImageUsage::Sampled};
}

std::shared_ptr<SwapchainGraphTarget> Target(std::uint64_t id = 1, std::uint32_t width = 64) {
    return std::make_shared<SwapchainGraphTarget>(id, TargetDescription(width));
}

GraphicsPassDescription ColorPass(GraphResourceId target,
                                  EAttachmentLoad load = EAttachmentLoad::CLEAR,
                                  EAttachmentStore store = EAttachmentStore::STORE) {
    GraphicsPassDescription pass;
    pass.Writes.push_back({target, EResourceUsage::COLOR_ATTACHMENT_WRITE});
    pass.ColorAttachments.push_back({.Resource = target, .Load = load, .Store = store});
    return pass;
}

// No GPU calls are needed to exercise Graphics ownership/generation validation.
class ResourceBackend final: public NGraphics::NBackend::IGraphicsBackend {
public:
    [[nodiscard]] const NGraphics::GraphicsCapabilities& GetCapabilities() const noexcept override {
        return m_capabilities;
    }
    std::uint64_t SubmitFrame(NGraphics::FrameSubmission) override {
        return 1;
    }
    [[nodiscard]] bool IsCompleted(std::uint64_t) const override {
        return true;
    }
    std::uint64_t CreateBuffer(const NGraphics::BufferDescriptor&) override {
        return 1;
    }
    void DestroyBuffer(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateGraphicsPipeline(const NGraphics::GraphicsPipelineDescriptor&) override {
        return 1;
    }
    void DestroyGraphicsPipeline(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateImage(const NGraphics::ImageDescriptor&) override {
        return m_nextImage++;
    }
    void DestroyImage(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateImageView(const NGraphics::ImageViewDescriptor&) override {
        return 1;
    }
    void DestroyImageView(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }
    std::uint64_t CreateSampler(const NGraphics::SamplerDescriptor&) override {
        return 1;
    }
    void DestroySampler(std::uint64_t, std::optional<std::uint64_t>) noexcept override {
    }

private:
    NGraphics::GraphicsCapabilities m_capabilities{
            .Presentation = true,
            .TimelineCompletion = true,
            .MaxFramesInFlight = 2,
    };
    std::uint64_t m_nextImage = 1;
};

NGraphics::Graphics MakeGraphics() {
    return NGraphics::NBackend::CreateGraphicsForBackend(std::make_unique<ResourceBackend>(), {});
}

NGraphics::ImageDescriptor ImageDescription() {
    return {.Extent = {64, 64, 1},
            .Usage = NGraphics::EImageUsage::ColorAttachment | NGraphics::EImageUsage::Sampled,
            .Access = NGraphics::EImageAccess::GpuRead | NGraphics::EImageAccess::GpuWrite};
}

TEST(RenderGraph, OneColorPassPublishesImmutableDrawPacketsAndTerminalPresent) {
    RenderGraphBuilder builder{FRAME};
    const auto target = Target();
    const auto image = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
    auto pass = ColorPass(image);
    NRenderer::DrawCommandData draw;
    draw.ObjectId = {42, 2};
    pass.DrawCommands.push_back(draw);
    const auto id = builder.AddGraphicsPass(pass);
    const auto graph = builder.Compile();
    pass.DrawCommands[0].ObjectId.Index = 99;
    ASSERT_EQ(graph.GetPasses().size(), 1);
    EXPECT_EQ(graph.GetPasses()[0].Id, id);
    EXPECT_EQ(graph.GetPasses()[0].Description.DrawCommands[0].ObjectId.Index, 42);
    EXPECT_EQ(graph.GetResources()[0].FinalUsage, EResourceUsage::PRESENT);
    EXPECT_EQ(graph.GetResource(image).Id, image);
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)graph.GetResource({}); });
    EXPECT_EQ(graph.GetFrame(), FRAME);
    EXPECT_NO_THROW(graph.ValidateResources(FRAME));
    (void)builder.AddGraphicsPass(ColorPass(image, EAttachmentLoad::LOAD));
    EXPECT_EQ(graph.GetPasses().size(), 1);
    EXPECT_EQ(builder.Compile().GetPasses().size(), 2);
}

TEST(RenderGraph, IndependentPassesHaveStableDeclarationOrderAndRepeatableCompile) {
    RenderGraphBuilder builder{FRAME};
    const auto a = Target(1);
    const auto b = Target(2);
    const auto first = builder.AddGraphicsPass(ColorPass(builder.ImportSwapchain(a, EResourceUsage::UNDEFINED)));
    const auto second = builder.AddGraphicsPass(ColorPass(builder.ImportSwapchain(b, EResourceUsage::PRESENT)));
    const auto one = builder.Compile();
    const auto two = builder.Compile();
    ASSERT_EQ(one.GetPasses().size(), 2);
    EXPECT_EQ(one.GetPasses()[0].Id, first);
    EXPECT_EQ(one.GetPasses()[1].Id, second);
    EXPECT_EQ(two.GetPasses()[0].Id, first);
    EXPECT_TRUE(one.GetDependencies().empty());
}

TEST(RenderGraph, ExplicitOrderReordersIndependentPassesAndDeduplicatesEdges) {
    RenderGraphBuilder builder{FRAME};
    const auto first = builder.AddGraphicsPass({});
    const auto second = builder.AddGraphicsPass({});
    const auto third = builder.AddGraphicsPass({});
    builder.AddDependency(third, first);
    builder.AddDependency(third, first);
    const auto graph = builder.Compile();
    EXPECT_EQ(graph.GetPasses()[0].Id, second);
    EXPECT_EQ(graph.GetPasses()[1].Id, third);
    EXPECT_EQ(graph.GetPasses()[2].Id, first);
    EXPECT_EQ(graph.GetDependencies().size(), 1);
}

TEST(RenderGraph, BuildsRawWarWawAndReadReadDoesNotAddDependencies) {
    auto graphics = MakeGraphics();
    const auto handle = graphics.CreateImage(ImageDescription());
    RenderGraphBuilder builder{FRAME};
    const auto image = builder.ImportImage(graphics, handle, EResourceUsage::UNDEFINED, EResourceUsage::SAMPLED_READ);
    const auto write = builder.AddGraphicsPass(ColorPass(image));
    GraphicsPassDescription sample;
    sample.Reads.push_back({image, EResourceUsage::SAMPLED_READ});
    const auto readA = builder.AddGraphicsPass(sample);
    const auto readB = builder.AddGraphicsPass(sample);
    const auto overwrite = builder.AddGraphicsPass(ColorPass(image));
    const auto graph = builder.Compile();
    const auto edges = graph.GetDependencies();
    EXPECT_EQ(edges.size(), 5);
    EXPECT_NE(std::ranges::find(edges, NRenderer::GraphDependency{write, readA}), edges.end());
    EXPECT_NE(std::ranges::find(edges, NRenderer::GraphDependency{write, readB}), edges.end());
    EXPECT_NE(std::ranges::find(edges, NRenderer::GraphDependency{write, overwrite}), edges.end());
    EXPECT_NE(std::ranges::find(edges, NRenderer::GraphDependency{readA, overwrite}), edges.end());
    EXPECT_NE(std::ranges::find(edges, NRenderer::GraphDependency{readB, overwrite}), edges.end());
}

TEST(RenderGraph, RejectsCyclesSelfDependenciesAndForeignPasses) {
    RenderGraphBuilder builder{FRAME};
    RenderGraphBuilder foreign{FRAME};
    const auto a = builder.AddGraphicsPass({});
    const auto b = builder.AddGraphicsPass({});
    const auto c = foreign.AddGraphicsPass({});
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { builder.AddDependency(a, a); });
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { builder.AddDependency(a, c); });
    builder.AddDependency(a, b);
    builder.AddDependency(b, a);
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
}

TEST(RenderGraph, RejectsReverseHazardEdge) {
    RenderGraphBuilder builder{FRAME};
    const auto target = Target();
    const auto image = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
    const auto a = builder.AddGraphicsPass(ColorPass(image));
    const auto b = builder.AddGraphicsPass(ColorPass(image));
    builder.AddDependency(b, a);
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
}

TEST(RenderGraph, RejectsReadAndLoadBeforeProduceAndDiscardedContents) {
    for (int scenario = 0; scenario < 3; ++scenario) {
        RenderGraphBuilder builder{FRAME};
        const auto target = Target();
        const auto image = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
        if (scenario == 0) {
            GraphicsPassDescription read;
            read.Reads.push_back({image, EResourceUsage::SAMPLED_READ});
            (void)builder.AddGraphicsPass(read);
        } else if (scenario == 1) {
            (void)builder.AddGraphicsPass(ColorPass(image, EAttachmentLoad::LOAD));
        } else {
            (void)builder.AddGraphicsPass(ColorPass(image, EAttachmentLoad::CLEAR, EAttachmentStore::DISCARD));
            (void)builder.AddGraphicsPass(ColorPass(image, EAttachmentLoad::LOAD));
        }
        ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
    }
}

TEST(RenderGraph, DiscardStoreCannotBePresentedButCanBeOverwritten) {
    RenderGraphBuilder builder{FRAME};
    const auto target = Target();
    const auto image = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
    (void)builder.AddGraphicsPass(ColorPass(image, EAttachmentLoad::CLEAR, EAttachmentStore::DISCARD));
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
    (void)builder.AddGraphicsPass(ColorPass(image));
    EXPECT_NO_THROW((void)builder.Compile());
}

TEST(RenderGraph, RejectsConflictingWritesReadsAndInvalidGraphResourceIds) {
    for (int scenario = 0; scenario < 5; ++scenario) {
        RenderGraphBuilder builder{FRAME};
        RenderGraphBuilder foreign{FRAME};
        const auto target = Target();
        const auto image = builder.ImportSwapchain(target, EResourceUsage::PRESENT);
        auto pass = ColorPass(image);
        if (scenario == 0) {
            pass.Writes.push_back(pass.Writes.front());
        }
        if (scenario == 1) {
            pass.Reads.push_back({image, EResourceUsage::SAMPLED_READ});
        }
        if (scenario == 2) {
            pass.Writes[0].Resource = {};
        }
        if (scenario == 3) {
            pass.Writes[0].Resource = foreign.ImportSwapchain(target, EResourceUsage::PRESENT);
        }
        if (scenario == 4) {
            pass.Writes[0].Usage = EResourceUsage::PRESENT;
        }
        (void)builder.AddGraphicsPass(pass);
        ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
    }
}

TEST(RenderGraph, RejectsInvalidAndDuplicateSwapchainImports) {
    RenderGraphBuilder builder{FRAME};
    const auto target = Target();
    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { (void)builder.ImportSwapchain(nullptr, EResourceUsage::PRESENT); });
    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { (void)builder.ImportSwapchain(target, EResourceUsage::SAMPLED_READ); });
    (void)builder.ImportSwapchain(target, EResourceUsage::PRESENT);
    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { (void)builder.ImportSwapchain(target, EResourceUsage::PRESENT); });
}

TEST(RenderGraph, InvalidatedOrExpiredSwapchainRejectsCompileAndRecording) {
    for (bool expire: {false, true}) {
        RenderGraphBuilder builder{FRAME};
        auto target = Target();
        const auto image = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
        (void)builder.AddGraphicsPass(ColorPass(image));
        const auto graph = builder.Compile();
        if (expire) {
            target.reset();
        } else {
            target->Invalidate();
        }
        ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
        ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { graph.ValidateResources(FRAME); });
    }
}

TEST(RenderGraph, FrameSlotGenerationCannotBeReused) {
    RenderGraphBuilder builder{FRAME};
    const auto graph = builder.Compile();
    auto recycled = FRAME;
    ++recycled.Generation;
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { graph.ValidateResources(recycled); });
    auto otherSlot = FRAME;
    ++otherSlot.FrameSlotIndex;
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { graph.ValidateResources(otherSlot); });
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [] { RenderGraphBuilder invalid{{}}; });
}

TEST(RenderGraph, ValidatesAttachmentMetadataAndPolicies) {
    for (int scenario = 0; scenario < 6; ++scenario) {
        RenderGraphBuilder builder{FRAME};
        const auto a = Target(1);
        const auto b = Target(2, scenario == 0 ? 32 : 64);
        const auto image = builder.ImportSwapchain(a, EResourceUsage::UNDEFINED);
        const auto other = builder.ImportSwapchain(b, EResourceUsage::PRESENT);
        auto pass = ColorPass(image);
        if (scenario == 0) {
            pass.Writes.push_back({other, EResourceUsage::COLOR_ATTACHMENT_WRITE});
            pass.ColorAttachments.push_back({.Resource = other});
        }
        if (scenario == 1) {
            pass.ColorAttachments.push_back(pass.ColorAttachments.front());
        }
        if (scenario == 2) {
            pass.ColorAttachments[0].Load = static_cast<EAttachmentLoad>(99);
        }
        if (scenario == 3) {
            pass.ColorAttachments[0].ClearColor[1] = std::numeric_limits<float>::quiet_NaN();
        }
        if (scenario == 4) {
            pass.ColorAttachments.clear();
        }
        if (scenario == 5) {
            pass.ColorAttachments[0].Store = static_cast<EAttachmentStore>(99);
        }
        (void)builder.AddGraphicsPass(pass);
        ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
    }
}

TEST(RenderGraph, RejectsInvalidExtentFormatSamplesAndCapabilityFlags) {
    for (int scenario = 0; scenario < 5; ++scenario) {
        auto description = TargetDescription();
        if (scenario == 0) {
            description.Extent.Width = 0;
        }
        if (scenario == 1) {
            description.Format = NGraphics::EPixelFormat::UNDEFINED;
        }
        if (scenario == 2) {
            description.Samples = static_cast<NGraphics::ESampleCount>(3);
        }
        if (scenario == 3) {
            description.Usage = 1U << 30U;
        }
        if (scenario == 4) {
            description.Format = NGraphics::EPixelFormat::D32_FLOAT;
        }
        ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { SwapchainGraphTarget invalid{1, description}; });
    }
    auto description = TargetDescription();
    description.Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled);
    const auto target = std::make_shared<SwapchainGraphTarget>(1, description);
    RenderGraphBuilder builder{FRAME};
    const auto image = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
    (void)builder.AddGraphicsPass(ColorPass(image));
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
}

TEST(RenderGraph, GraphicsImportsValidateOwnerGenerationCapabilitiesAndTerminalUsage) {
    auto graphics = MakeGraphics();
    auto other = MakeGraphics();
    const auto image = graphics.CreateImage(ImageDescription());
    RenderGraphBuilder builder{FRAME};
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)builder.ImportImage(other, image, EResourceUsage::UNDEFINED, EResourceUsage::SAMPLED_READ);
    });
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)builder.ImportImage(graphics, image, EResourceUsage::UNDEFINED, EResourceUsage::PRESENT);
    });
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)builder.ImportImage(graphics, image, EResourceUsage::UNDEFINED, EResourceUsage::UNDEFINED);
    });
    const auto id = builder.ImportImage(graphics, image, EResourceUsage::SAMPLED_READ, EResourceUsage::SAMPLED_READ);
    (void)builder.AddGraphicsPass(ColorPass(id));
    const auto graph = builder.Compile();
    graphics.DestroyImage(image);
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { graph.ValidateResources(FRAME); });
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
}

TEST(RenderGraph, BufferImportSupportsSemanticReadsAndTracksRecycledHandle) {
    auto graphics = MakeGraphics();
    const NGraphics::BufferDescriptor description{
            .SizeBytes = 64,
            .Usage = NGraphics::EBufferUsage::Vertex | NGraphics::EBufferUsage::TransferDestination,
            .Access = NGraphics::EBufferAccess::GpuRead | NGraphics::EBufferAccess::GpuWrite};
    const auto buffer = graphics.CreateBuffer(description);
    RenderGraphBuilder builder{FRAME};
    const auto id = builder.ImportBuffer(graphics, buffer, EResourceUsage::VERTEX_READ, EResourceUsage::VERTEX_READ);
    GraphicsPassDescription pass;
    pass.Reads.push_back({id, EResourceUsage::VERTEX_READ});
    (void)builder.AddGraphicsPass(pass);
    const auto graph = builder.Compile();
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)builder.ImportBuffer(graphics, buffer, EResourceUsage::VERTEX_READ, EResourceUsage::VERTEX_READ);
    });
    graphics.DestroyBuffer(buffer);
    const auto replacement = graphics.CreateBuffer(description);
    EXPECT_NE(replacement, buffer);
    EXPECT_EQ(replacement.GetValue(), buffer.GetValue());
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { graph.ValidateResources(FRAME); });
}

TEST(RenderGraph, RejectsImageBufferUsageMismatchAndUnknownEnum) {
    auto graphics = MakeGraphics();
    const auto image = graphics.CreateImage(ImageDescription());
    for (const auto usage: {EResourceUsage::VERTEX_READ, static_cast<EResourceUsage>(99)}) {
        RenderGraphBuilder builder{FRAME};
        ExpectError(NCommon::EError::INVALID_ARGUMENT,
                    [&] { (void)builder.ImportImage(graphics, image, usage, EResourceUsage::SAMPLED_READ); });
    }
}

TEST(RenderGraph, DepthAndColorAttachmentsCompileTogetherWithMatchingMetadata) {
    auto graphics = MakeGraphics();
    auto depthDescription = ImageDescription();
    depthDescription.Format = NGraphics::EImageFormat::D32_FLOAT;
    depthDescription.Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::DepthStencilAttachment);
    const auto depthHandle = graphics.CreateImage(depthDescription);
    RenderGraphBuilder builder{FRAME};
    const auto target = Target();
    const auto color = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
    const auto depth =
            builder.ImportImage(graphics, depthHandle, EResourceUsage::UNDEFINED, EResourceUsage::DEPTH_STENCIL_WRITE);
    auto pass = ColorPass(color);
    pass.Writes.push_back({depth, EResourceUsage::DEPTH_STENCIL_WRITE});
    pass.DepthAttachment = NRenderer::GraphAttachment{.Resource = depth};
    (void)builder.AddGraphicsPass(pass);
    const auto graph = builder.Compile();
    ASSERT_TRUE(graph.GetPasses()[0].Description.DepthAttachment.has_value());
    EXPECT_EQ(graph.GetResource(depth).ImageDescription.Format, NGraphics::EPixelFormat::D32_FLOAT);
}

TEST(RenderGraph, RejectsLoadWithoutGpuReadAccess) {
    auto graphics = MakeGraphics();
    auto description = ImageDescription();
    description.Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuWrite);
    const auto handle = graphics.CreateImage(description);
    RenderGraphBuilder builder{FRAME};
    const auto image = builder.ImportImage(graphics,
                                           handle,
                                           EResourceUsage::COLOR_ATTACHMENT_WRITE,
                                           EResourceUsage::COLOR_ATTACHMENT_WRITE);
    (void)builder.AddGraphicsPass(ColorPass(image, EAttachmentLoad::LOAD));
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
}

TEST(RenderGraph, SimultaneousFrameSlotsAndDestroyedBuilderKeepIndependentPlans) {
    const auto target = Target();
    const auto create = [&target](NRenderer::RenderFrameIdentity frame) {
        RenderGraphBuilder builder{frame};
        const auto image = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
        (void)builder.AddGraphicsPass(ColorPass(image));
        return builder.Compile();
    };
    const auto first = create(FRAME);
    auto next = FRAME;
    ++next.FrameSlotIndex;
    ++next.ApplicationFrameIndex;
    const auto second = create(next);
    EXPECT_NO_THROW(first.ValidateResources(FRAME));
    EXPECT_NO_THROW(second.ValidateResources(next));
    EXPECT_NE(first.GetPasses()[0].Id, second.GetPasses()[0].Id);
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)first.GetResource(second.GetResources()[0].Id); });
}

} // namespace
