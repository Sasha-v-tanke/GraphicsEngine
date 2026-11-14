#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <utility>

#include <GraphicsEngine/renderer/render_graph.h>
#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <graphics/graphics.h>
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

GraphicsPassDescription SideEffectPass(bool hasSideEffects = true) {
    GraphicsPassDescription pass;
    pass.HasSideEffects = hasSideEffects;
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
    const auto first = builder.AddGraphicsPass(SideEffectPass());
    const auto second = builder.AddGraphicsPass(SideEffectPass());
    const auto third = builder.AddGraphicsPass(SideEffectPass());
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
    sample.HasSideEffects = true;
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
    const auto a = builder.AddGraphicsPass(SideEffectPass());
    const auto b = builder.AddGraphicsPass(SideEffectPass());
    const auto c = foreign.AddGraphicsPass(SideEffectPass());
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
    pass.HasSideEffects = true;
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

TEST(RenderGraph, DiscardLoadCannotInitializeOrPreserveWholeAttachmentContents) {
    for (const bool hasDraws: {false, true}) {
        for (const EResourceUsage initial: {EResourceUsage::UNDEFINED, EResourceUsage::PRESENT}) {
            RenderGraphBuilder builder{FRAME};
            const auto target = Target();
            const auto image = builder.ImportSwapchain(target, initial);
            auto pass = ColorPass(image, EAttachmentLoad::DISCARD);
            if (hasDraws) {
                pass.DrawCommands.emplace_back();
            }
            (void)builder.AddGraphicsPass(pass);
            ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
        }
    }
}

TEST(RenderGraph, ReadAfterDiscardLoadRequiresSubsequentClear) {
    auto graphics = MakeGraphics();
    const auto handle = graphics.CreateImage(ImageDescription());
    RenderGraphBuilder builder{FRAME};
    const auto image =
            builder.ImportImage(graphics, handle, EResourceUsage::SAMPLED_READ, EResourceUsage::SAMPLED_READ);
    (void)builder.AddGraphicsPass(ColorPass(image, EAttachmentLoad::DISCARD));
    GraphicsPassDescription sample;
    sample.HasSideEffects = true;
    sample.Reads.push_back({image, EResourceUsage::SAMPLED_READ});
    (void)builder.AddGraphicsPass(sample);
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
}

TEST(RenderGraph, DiscardLoadFollowedByClearProducesDefinedOutput) {
    RenderGraphBuilder builder{FRAME};
    const auto target = Target();
    const auto image = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
    (void)builder.AddGraphicsPass(ColorPass(image, EAttachmentLoad::DISCARD));
    (void)builder.AddGraphicsPass(ColorPass(image, EAttachmentLoad::CLEAR));
    EXPECT_NO_THROW((void)builder.Compile());
}

TEST(RenderGraph, RejectsMultiMipImportUntilSubresourceTrackingIsSupported) {
    auto graphics = MakeGraphics();
    auto description = ImageDescription();
    description.MipLevels = 2;
    const auto image = graphics.CreateImage(description);
    RenderGraphBuilder builder{FRAME};
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)builder.ImportImage(graphics, image, EResourceUsage::UNDEFINED, EResourceUsage::SAMPLED_READ);
    });
}

TEST(RenderGraph, TransferDestinationStateIsNotAWritePayload) {
    auto graphics = MakeGraphics();
    const NGraphics::BufferDescriptor description{
            .SizeBytes = 64,
            .Usage = NGraphics::EBufferUsage::Vertex | NGraphics::EBufferUsage::TransferDestination,
            .Access = NGraphics::EBufferAccess::GpuRead | NGraphics::EBufferAccess::GpuWrite,
    };
    const auto handle = graphics.CreateBuffer(description);
    RenderGraphBuilder builder{FRAME};
    const auto buffer = builder.ImportBuffer(graphics, handle, EResourceUsage::UNDEFINED, EResourceUsage::VERTEX_READ);
    GraphicsPassDescription transfer;
    transfer.Writes.push_back({buffer, EResourceUsage::TRANSFER_DESTINATION});
    (void)builder.AddGraphicsPass(transfer);
    GraphicsPassDescription read;
    read.Reads.push_back({buffer, EResourceUsage::VERTEX_READ});
    (void)builder.AddGraphicsPass(read);
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)builder.Compile(); });
}

TEST(RenderGraph, CullsDeadTransientBranchesAndOverwrittenImportedOutput) {
    RenderGraphBuilder builder{FRAME};
    const auto target = Target();
    const auto output = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
    const auto dead = builder.CreateTransientImage(TargetDescription());
    const auto live = builder.CreateTransientImage(TargetDescription());
    (void)builder.AddGraphicsPass(ColorPass(dead));
    (void)builder.AddGraphicsPass(ColorPass(output));
    const auto producer = builder.AddGraphicsPass(ColorPass(live));
    auto final = ColorPass(output);
    final.Reads.push_back({live, EResourceUsage::SAMPLED_READ});
    const auto consumer = builder.AddGraphicsPass(final);
    const auto graph = builder.Compile();
    ASSERT_EQ(graph.GetPasses().size(), 2);
    EXPECT_EQ(graph.GetPasses()[0].Id, producer);
    EXPECT_EQ(graph.GetPasses()[1].Id, consumer);
    EXPECT_EQ(graph.GetResources().size(), 2);
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)graph.GetResource(dead); });
    EXPECT_EQ(graph.GetResource(live).Lifetime, NRenderer::EGraphResourceLifetime::TRANSIENT);
    EXPECT_EQ(graph.GetDependencies().size(), 1);
}

TEST(RenderGraph, PoolsOnlyCompatibleDisjointLifetimesAndAddsReuseOrdering) {
    RenderGraphBuilder builder{FRAME};
    const auto first = builder.CreateTransientImage(TargetDescription());
    const auto overlapping = builder.CreateTransientImage(TargetDescription());
    const auto reused = builder.CreateTransientImage(TargetDescription());
    const auto different = builder.CreateTransientImage(TargetDescription(32));
    auto firstPass = ColorPass(first);
    firstPass.HasSideEffects = true;
    const auto a = builder.AddGraphicsPass(firstPass);
    auto secondPass = ColorPass(overlapping);
    secondPass.Reads.push_back({first, EResourceUsage::SAMPLED_READ});
    secondPass.HasSideEffects = true;
    const auto b = builder.AddGraphicsPass(secondPass);
    auto thirdPass = ColorPass(reused);
    thirdPass.HasSideEffects = true;
    const auto c = builder.AddGraphicsPass(thirdPass);
    auto fourthPass = ColorPass(different);
    fourthPass.HasSideEffects = true;
    (void)builder.AddGraphicsPass(fourthPass);
    const auto graph = builder.Compile();
    const auto lifetimes = graph.GetLifetimes();
    ASSERT_EQ(lifetimes.size(), 4);
    EXPECT_EQ(lifetimes[0].FirstPass, 0);
    EXPECT_EQ(lifetimes[0].LastPass, 1);
    EXPECT_NE(lifetimes[0].Allocation, lifetimes[1].Allocation);
    EXPECT_EQ(lifetimes[0].Allocation, lifetimes[2].Allocation);
    EXPECT_NE(lifetimes[0].Allocation, lifetimes[3].Allocation);
    EXPECT_NE(std::ranges::find(graph.GetDependencies(), NRenderer::GraphDependency{a, b}),
              graph.GetDependencies().end());
    EXPECT_NE(std::ranges::find(graph.GetDependencies(), NRenderer::GraphDependency{b, c}),
              graph.GetDependencies().end());
}

TEST(RenderGraph, LoadAndExplicitDependenciesRetainProducersButClearDoesNot) {
    RenderGraphBuilder builder{FRAME};
    const auto target = Target();
    const auto output = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
    const auto discarded = builder.AddGraphicsPass(ColorPass(output));
    const auto prerequisite = builder.AddGraphicsPass({});
    const auto clear = builder.AddGraphicsPass(ColorPass(output));
    const auto load = builder.AddGraphicsPass(ColorPass(output, EAttachmentLoad::LOAD));
    builder.AddDependency(prerequisite, load);
    const auto graph = builder.Compile();
    ASSERT_EQ(graph.GetPasses().size(), 3);
    EXPECT_EQ(graph.GetPasses()[0].Id, prerequisite);
    EXPECT_EQ(graph.GetPasses()[1].Id, clear);
    EXPECT_EQ(graph.GetPasses()[2].Id, load);
    EXPECT_TRUE(std::ranges::none_of(graph.GetPasses(), [&](const auto& pass) { return pass.Id == discarded; }));
    const auto transitions = graph.GetTransitions();
    ASSERT_EQ(transitions.size(), 3);
    EXPECT_EQ(transitions[0].Before, EResourceUsage::UNDEFINED);
    EXPECT_FALSE(transitions[0].ReadsAttachment);
    EXPECT_EQ(transitions[1].Before, EResourceUsage::COLOR_ATTACHMENT_WRITE);
    EXPECT_TRUE(transitions[1].ReadsAttachment);
    EXPECT_EQ(transitions[2].After, EResourceUsage::PRESENT);
    EXPECT_EQ(transitions[2].PassIndex, graph.GetPasses().size());
}

TEST(RenderGraph, RecordingRangesPartitionDrawsAndStayStableAcrossRepeatedCompile) {
    RenderGraphBuilder builder{FRAME};
    const auto target = Target();
    const auto output = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
    auto pass = ColorPass(output);
    pass.DrawCommands.resize(155);
    (void)builder.AddGraphicsPass(pass);
    const auto graph = builder.Compile();
    const auto repeated = builder.Compile();
    const auto ranges = graph.GetRecordingRanges();
    ASSERT_EQ(ranges.size(), 3);
    std::size_t next = 0;
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        EXPECT_EQ(ranges[index].PassIndex, 0);
        EXPECT_EQ(ranges[index].FirstDraw, next);
        EXPECT_LE(ranges[index].DrawCount, 64);
        EXPECT_EQ(ranges[index].FirstDraw, repeated.GetRecordingRanges()[index].FirstDraw);
        next += ranges[index].DrawCount;
    }
    EXPECT_EQ(next, pass.DrawCommands.size());
}

TEST(RenderGraph, UnusedImportedResourcesAreNotValidatedAtExecutionAndNeverPooled) {
    auto graphics = MakeGraphics();
    const auto handle = graphics.CreateImage(ImageDescription());
    RenderGraphBuilder builder{FRAME};
    const auto unused =
            builder.ImportImage(graphics, handle, EResourceUsage::SAMPLED_READ, EResourceUsage::SAMPLED_READ);
    const auto target = Target();
    const auto output = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
    (void)builder.AddGraphicsPass(ColorPass(output));
    const auto graph = builder.Compile();
    graphics.DestroyImage(handle);
    EXPECT_NO_THROW(graph.ValidateResources(FRAME));
    EXPECT_EQ(graph.GetResources().size(), 1);
    EXPECT_FALSE(graph.GetLifetimes()[0].Allocation);
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)graph.GetResource(unused); });
}

TEST(RenderGraph, DeepExplicitDagCullingIsIterativeAndDeterministic) {
    RenderGraphBuilder builder{FRAME};
    auto previous = builder.AddGraphicsPass({});
    for (std::size_t index = 0; index < 10000; ++index) {
        const auto next = builder.AddGraphicsPass(SideEffectPass(index == 9999));
        builder.AddDependency(previous, next);
        previous = next;
    }
    const auto graph = builder.Compile();
    ASSERT_EQ(graph.GetPasses().size(), 10001);
    EXPECT_EQ(graph.GetPasses().back().Id, previous);
    EXPECT_EQ(graph.GetDependencies().size(), 10000);
}

TEST(RenderGraph, SeededRandomDagMatchesReferenceReachabilityAndPoolIntervals) {
    // Reproducible scheduler replay, not a source of security randomness.
    // NOLINTNEXTLINE(cert-msc51-cpp)
    std::mt19937 random{6317};
    for (std::size_t iteration = 0; iteration < 40; ++iteration) {
        SCOPED_TRACE(iteration);
        RenderGraphBuilder builder{FRAME};
        std::vector<GraphResourceId> images;
        std::vector<NRenderer::GraphPassId> passes;
        std::vector<std::vector<std::size_t>> parents(64);
        for (std::size_t index = 0; index < 64; ++index) {
            images.push_back(builder.CreateTransientImage(TargetDescription()));
            auto pass = ColorPass(images.back());
            if (index > 0) {
                const std::size_t parent = random() % index;
                parents[index].push_back(parent);
                pass.Reads.push_back({images[parent], EResourceUsage::SAMPLED_READ});
            }
            passes.push_back(builder.AddGraphicsPass(pass));
        }
        const auto target = Target();
        const auto output = builder.ImportSwapchain(target, EResourceUsage::UNDEFINED);
        auto final = ColorPass(output);
        final.Reads.push_back({images.back(), EResourceUsage::SAMPLED_READ});
        (void)builder.AddGraphicsPass(final);
        std::vector<bool> expected(64);
        std::vector<std::size_t> pending{63};
        while (!pending.empty()) {
            const std::size_t index = pending.back();
            pending.pop_back();
            if (!expected[index]) {
                expected[index] = true;
                pending.insert(pending.end(), parents[index].begin(), parents[index].end());
            }
        }
        const auto graph = builder.Compile();
        EXPECT_EQ(graph.GetPasses().size(), static_cast<std::size_t>(std::ranges::count(expected, true)) + 1);
        for (std::size_t index = 0; index < passes.size(); ++index) {
            EXPECT_EQ(
                    std::ranges::any_of(graph.GetPasses(), [&](const auto& pass) { return pass.Id == passes[index]; }),
                    expected[index]);
        }
        const auto lifetimes = graph.GetLifetimes();
        for (std::size_t a = 0; a < lifetimes.size(); ++a) {
            for (std::size_t b = a + 1; b < lifetimes.size(); ++b) {
                if (lifetimes[a].Allocation && lifetimes[a].Allocation == lifetimes[b].Allocation) {
                    EXPECT_TRUE(lifetimes[a].LastPass < lifetimes[b].FirstPass ||
                                lifetimes[b].LastPass < lifetimes[a].FirstPass);
                }
            }
        }
    }
}

TEST(RenderGraph, CullingInterveningWriteRebuildsWarBetweenSurvivingPasses) {
    auto graphics = MakeGraphics();
    const auto image = graphics.CreateImage(ImageDescription());
    RenderGraphBuilder builder{FRAME};
    const auto imported =
            builder.ImportImage(graphics, image, EResourceUsage::SAMPLED_READ, EResourceUsage::SAMPLED_READ);
    GraphicsPassDescription read;
    read.Reads = {{imported, EResourceUsage::SAMPLED_READ}};
    read.HasSideEffects = true;
    const auto reader = builder.AddGraphicsPass(read);
    const auto dead = builder.AddGraphicsPass(ColorPass(imported));
    const auto writer = builder.AddGraphicsPass(ColorPass(imported));
    const auto graph = builder.Compile();
    ASSERT_EQ(graph.GetPasses().size(), 2);
    EXPECT_EQ(graph.GetPasses()[0].Id, reader);
    EXPECT_EQ(graph.GetPasses()[1].Id, writer);
    EXPECT_NE(std::ranges::find(graph.GetDependencies(), NRenderer::GraphDependency{reader, writer}),
              graph.GetDependencies().end());
    EXPECT_TRUE(std::ranges::none_of(graph.GetDependencies(),
                                     [&](const auto& edge) { return edge.Before == dead || edge.After == dead; }));
}

TEST(RenderGraph, PoolReuseWaitsForEveryIndependentReader) {
    RenderGraphBuilder builder{FRAME};
    const auto first = builder.CreateTransientImage(TargetDescription());
    const auto next = builder.CreateTransientImage(TargetDescription());
    (void)builder.AddGraphicsPass(ColorPass(first));
    GraphicsPassDescription read;
    read.Reads = {{first, EResourceUsage::SAMPLED_READ}};
    read.HasSideEffects = true;
    const auto a = builder.AddGraphicsPass(read);
    const auto b = builder.AddGraphicsPass(read);
    auto write = ColorPass(next);
    write.HasSideEffects = true;
    const auto reuse = builder.AddGraphicsPass(write);
    const auto graph = builder.Compile();
    EXPECT_EQ(graph.GetLifetimes()[0].Allocation, graph.GetLifetimes()[1].Allocation);
    EXPECT_NE(std::ranges::find(graph.GetDependencies(), NRenderer::GraphDependency{a, reuse}),
              graph.GetDependencies().end());
    EXPECT_NE(std::ranges::find(graph.GetDependencies(), NRenderer::GraphDependency{b, reuse}),
              graph.GetDependencies().end());
}

} // namespace
