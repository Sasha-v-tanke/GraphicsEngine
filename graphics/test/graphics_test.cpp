#include <memory>
#include <optional>
#include <vector>

#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <graphics/graphics.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <tests/common/test_error.h>

namespace {

using NTest::ExpectError;

struct FakeGraphicsState {
    NGraphics::GraphicsCapabilities Capabilities{
            .Presentation = true,
            .TimelineCompletion = true,
            .MaxFramesInFlight = 2,
    };
    int CreatedCount = 0;
    int DestroyedCount = 0;
    std::vector<NGraphics::FrameSubmission> Submissions;
    std::vector<NGraphics::BufferDescriptor> CreatedBuffers;
    std::vector<std::pair<std::uint64_t, std::optional<std::uint64_t>>> DestroyedBuffers;
    std::vector<NGraphics::GraphicsPipelineDescriptor> CreatedGraphicsPipelines;
    std::vector<std::pair<std::uint64_t, std::optional<std::uint64_t>>> DestroyedGraphicsPipelines;
    std::uint64_t NextBuffer = 1;
    std::uint64_t NextGraphicsPipeline = 1;
    std::uint64_t NextCompletion = 1;
    std::uint64_t CompletedValue = 0;
};

thread_local FakeGraphicsState* g_fakeGraphicsState = nullptr;

class FakeGraphicsBackend final: public NGraphics::NBackend::IGraphicsBackend {
public:
    explicit FakeGraphicsBackend(FakeGraphicsState& state)
        : m_state(state) {
        ++m_state.CreatedCount;
    }

    ~FakeGraphicsBackend() override {
        ++m_state.DestroyedCount;
    }

    [[nodiscard]] const NGraphics::GraphicsCapabilities& GetCapabilities() const noexcept override {
        return m_state.Capabilities;
    }

    [[nodiscard]] std::uint64_t SubmitFrame(const NGraphics::FrameSubmission& submission) override {
        m_state.Submissions.push_back(submission);
        return m_state.NextCompletion++;
    }

    [[nodiscard]] bool IsCompleted(std::uint64_t completionValue) const override {
        return completionValue <= m_state.CompletedValue;
    }

    [[nodiscard]] std::uint64_t CreateBuffer(const NGraphics::BufferDescriptor& descriptor) override {
        m_state.CreatedBuffers.push_back(descriptor);
        return m_state.NextBuffer++;
    }

    void DestroyBuffer(std::uint64_t bufferValue, std::optional<std::uint64_t> completedAfter) noexcept override {
        m_state.DestroyedBuffers.emplace_back(bufferValue, completedAfter);
    }

    [[nodiscard]] std::uint64_t
    CreateGraphicsPipeline(const NGraphics::GraphicsPipelineDescriptor& descriptor) override {
        m_state.CreatedGraphicsPipelines.push_back(descriptor);
        return m_state.NextGraphicsPipeline++;
    }

    void DestroyGraphicsPipeline(std::uint64_t pipelineValue,
                                 std::optional<std::uint64_t> completedAfter) noexcept override {
        m_state.DestroyedGraphicsPipelines.emplace_back(pipelineValue, completedAfter);
    }

private:
    FakeGraphicsState& m_state;
};

[[nodiscard]] NGraphics::Shader MakeShader(NGraphics::EShaderStage stage, std::uint32_t payload) {
    return NGraphics::Shader{
            stage,
            NGraphics::ShaderArtifact{
                    .Format = NGraphics::EShaderArtifactFormat::SPIR_V,
                    .Words = {0x07230203U, 0x00010000U, 0U, 2U, 0U, payload},
            },
    };
}

[[nodiscard]] NGraphics::GraphicsPipelineDescriptor MakeGraphicsPipelineDescriptor() {
    return {
            .Shaders = {
                    MakeShader(NGraphics::EShaderStage::VERTEX, 1U),
                    MakeShader(NGraphics::EShaderStage::FRAGMENT, 2U),
            },
            .VertexLayout =
                    {
                            .Bindings =
                                    {
                                            {
                                                    .Binding = 0,
                                                    .StrideBytes = 20,
                                                    .InputRate = NGraphics::EVertexInputRate::VERTEX,
                                            },
                                    },
                            .Attributes =
                                    {
                                            {
                                                    .Location = 0,
                                                    .Binding = 0,
                                                    .Format = NGraphics::EVertexFormat::FLOAT32_3,
                                                    .OffsetBytes = 0,
                                            },
                                            {
                                                    .Location = 1,
                                                    .Binding = 0,
                                                    .Format = NGraphics::EVertexFormat::FLOAT32_2,
                                                    .OffsetBytes = 12,
                                            },
                                    },
                    },
            .Topology = NGraphics::EPrimitiveTopology::TRIANGLE_LIST,
            .RasterState =
                    {
                            .PolygonMode = NGraphics::EPolygonMode::FILL,
                            .CullMode = NGraphics::ECullMode::BACK,
                            .FrontFace = NGraphics::EFrontFace::COUNTER_CLOCKWISE,
                    },
            .DepthState =
                    {
                            .TestEnabled = false,
                            .WriteEnabled = false,
                            .CompareOperation = NGraphics::ECompareOperation::LESS,
                    },
            .ColorAttachmentFormats = {NGraphics::EPixelFormat::BGRA8_SRGB},
            .ColorBlendAttachments = {NGraphics::BlendAttachmentDescriptor{}},
            .DepthAttachmentFormat = NGraphics::EPixelFormat::UNDEFINED,
            .Samples = NGraphics::ESampleCount::X1,
    };
}

class GraphicsTest: public testing::Test {
protected:
    void SetUp() override {
        g_fakeGraphicsState = &m_state;
    }

    void TearDown() override {
        g_fakeGraphicsState = nullptr;
    }

    [[nodiscard]] static NGraphics::Graphics
    CreateGraphics(const NGraphics::RequiredGraphicsCapabilities& requiredCapabilities = {}) {
        return NGraphics::NBackend::CreateGraphicsForBackend(
                std::make_unique<FakeGraphicsBackend>(*g_fakeGraphicsState),
                requiredCapabilities);
    }

    FakeGraphicsState m_state;
};

TEST_F(GraphicsTest, CreatesBackendAndPublishesImmutableCapabilities) {
    {
        const NGraphics::Graphics graphics = CreateGraphics();

        EXPECT_EQ(m_state.CreatedCount, 1);
        EXPECT_EQ(m_state.DestroyedCount, 0);
        EXPECT_TRUE(graphics.GetCapabilities().Presentation);
        EXPECT_TRUE(graphics.GetCapabilities().TimelineCompletion);
        EXPECT_EQ(graphics.GetCapabilities().MaxFramesInFlight, 2U);
    }

    EXPECT_EQ(m_state.DestroyedCount, 1);
}

TEST_F(GraphicsTest, RejectsBackendThatDoesNotSatisfyRequiredCapabilities) {
    m_state.Capabilities.TimelineCompletion = false;

    NGraphics::RequiredGraphicsCapabilities requiredCapabilities;
    requiredCapabilities.TimelineCompletion = true;

    ExpectError(NCommon::EError::UNSUPPORTED,
                [&] { const NGraphics::Graphics graphics = CreateGraphics(requiredCapabilities); });

    EXPECT_EQ(m_state.CreatedCount, 1);
    EXPECT_EQ(m_state.DestroyedCount, 1);
}

TEST_F(GraphicsTest, SubmitsFrameAndReportsCompletionPoint) {
    NGraphics::Graphics graphics = CreateGraphics();

    const auto completion = graphics.SubmitFrame({
            .FrameIndex = 42,
            .RequiresPresentation = true,
    });

    ASSERT_TRUE(completion.IsValid());
    ASSERT_EQ(m_state.Submissions.size(), 1U);
    EXPECT_EQ(m_state.Submissions[0].FrameIndex, 42U);
    EXPECT_TRUE(m_state.Submissions[0].RequiresPresentation);
    EXPECT_FALSE(graphics.IsCompleted(completion));

    m_state.CompletedValue = completion.GetValue();

    EXPECT_TRUE(graphics.IsCompleted(completion));
}

TEST_F(GraphicsTest, RejectsPresentationSubmissionWhenBackendHasNoPresentationPath) {
    m_state.Capabilities.Presentation = false;

    NGraphics::RequiredGraphicsCapabilities requiredCapabilities;
    requiredCapabilities.Presentation = false;

    NGraphics::Graphics graphics = CreateGraphics(requiredCapabilities);

    ExpectError(NCommon::EError::UNSUPPORTED, [&] {
        (void)graphics.SubmitFrame({
                .FrameIndex = 1,
                .RequiresPresentation = true,
        });
    });
}

TEST_F(GraphicsTest, RejectsInvalidCompletionPoint) {
    const NGraphics::Graphics graphics = CreateGraphics();

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)graphics.IsCompleted(NGraphics::CompletionPoint{}); });
}

TEST_F(GraphicsTest, RejectsCompletionPointFromAnotherGraphics) {
    NGraphics::Graphics first = CreateGraphics();

    FakeGraphicsState otherState;
    g_fakeGraphicsState = &otherState;
    NGraphics::Graphics second = CreateGraphics();
    g_fakeGraphicsState = &m_state;

    const auto completion = first.SubmitFrame({
            .FrameIndex = 1,
            .RequiresPresentation = true,
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)second.IsCompleted(completion); });
}

TEST_F(GraphicsTest, CreatesBufferAndPreservesDescriptor) {
    NGraphics::Graphics graphics = CreateGraphics();

    const NGraphics::BufferDescriptor descriptor{
            .SizeBytes = 4096,
            .Usage = NGraphics::EBufferUsage::TransferDestination | NGraphics::EBufferUsage::Vertex,
            .Access = NGraphics::EBufferAccess::CpuWrite | NGraphics::EBufferAccess::GpuRead,
            .Lifetime = NGraphics::EBufferLifetime::Persistent,
    };

    const NGraphics::BufferHandle buffer = graphics.CreateBuffer(descriptor);

    ASSERT_TRUE(buffer.IsValid());
    ASSERT_EQ(m_state.CreatedBuffers.size(), 1U);
    EXPECT_EQ(m_state.CreatedBuffers[0].SizeBytes, descriptor.SizeBytes);
    EXPECT_EQ(m_state.CreatedBuffers[0].Usage, descriptor.Usage);
    EXPECT_EQ(m_state.CreatedBuffers[0].Access, descriptor.Access);
    EXPECT_EQ(m_state.CreatedBuffers[0].Lifetime, descriptor.Lifetime);
    EXPECT_EQ(graphics.GetBufferDescriptor(buffer).SizeBytes, descriptor.SizeBytes);
}

TEST_F(GraphicsTest, RejectsInvalidBufferDescriptor) {
    NGraphics::Graphics graphics = CreateGraphics();

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateBuffer({
                .SizeBytes = 0,
                .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::Vertex),
                .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::GpuRead),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateBuffer({
                .SizeBytes = 1,
                .Usage = 0,
                .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::GpuRead),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateBuffer({
                .SizeBytes = 1,
                .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::Vertex),
                .Access = 0,
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateBuffer({
                .SizeBytes = 1,
                .Usage = 0x80000000U,
                .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::GpuRead),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateBuffer({
                .SizeBytes = 1,
                .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::Vertex),
                .Access = 0x40000000U,
        });
    });
}

TEST_F(GraphicsTest, DestroysBufferAfterCompletionPoint) {
    NGraphics::Graphics graphics = CreateGraphics();

    const NGraphics::BufferHandle buffer = graphics.CreateBuffer({
            .SizeBytes = 16,
            .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::Storage),
            .Access = NGraphics::EBufferAccess::GpuRead | NGraphics::EBufferAccess::GpuWrite,
            .Lifetime = NGraphics::EBufferLifetime::FrameLocal,
    });

    const NGraphics::CompletionPoint completion = graphics.SubmitFrame({
            .FrameIndex = 7,
            .RequiresPresentation = false,
    });

    graphics.DestroyBuffer(buffer, completion);

    ASSERT_EQ(m_state.DestroyedBuffers.size(), 1U);
    EXPECT_EQ(m_state.DestroyedBuffers[0].first, buffer.GetValue());
    ASSERT_TRUE(m_state.DestroyedBuffers[0].second.has_value());
    EXPECT_EQ(*m_state.DestroyedBuffers[0].second, completion.GetValue());
}

TEST_F(GraphicsTest, RejectsInvalidForeignAndStaleBufferHandles) {
    NGraphics::Graphics graphics = CreateGraphics();

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { graphics.DestroyBuffer(NGraphics::BufferHandle{}); });

    const NGraphics::BufferHandle buffer = graphics.CreateBuffer({
            .SizeBytes = 32,
            .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::Uniform),
            .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::GpuRead),
    });

    FakeGraphicsState otherState;
    g_fakeGraphicsState = &otherState;
    NGraphics::Graphics other = CreateGraphics();
    g_fakeGraphicsState = &m_state;

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { other.DestroyBuffer(buffer); });

    graphics.DestroyBuffer(buffer);

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)graphics.GetBufferDescriptor(buffer); });
}

TEST_F(GraphicsTest, RollsBackBackendBufferWhenPublicationFails) {
    NGraphics::Graphics graphics = CreateGraphics();

    const NGraphics::BufferHandle first = graphics.CreateBuffer({
            .SizeBytes = 32,
            .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::Uniform),
            .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::GpuRead),
    });

    m_state.NextBuffer = first.GetValue();

    ExpectError(NCommon::EError::INVALID_STATE, [&] {
        (void)graphics.CreateBuffer({
                .SizeBytes = 64,
                .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::Storage),
                .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::GpuRead),
        });
    });

    ASSERT_EQ(m_state.DestroyedBuffers.size(), 1U);
    EXPECT_EQ(m_state.DestroyedBuffers[0].first, first.GetValue());
    EXPECT_FALSE(m_state.DestroyedBuffers[0].second.has_value());
    EXPECT_EQ(graphics.GetBufferDescriptor(first).SizeBytes, 32U);
}

TEST_F(GraphicsTest, ReturnsBufferDescriptorByValue) {
    NGraphics::Graphics graphics = CreateGraphics();

    const NGraphics::BufferHandle buffer = graphics.CreateBuffer({
            .SizeBytes = 256,
            .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::Vertex),
            .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::GpuRead),
    });

    const NGraphics::BufferDescriptor descriptor = graphics.GetBufferDescriptor(buffer);

    graphics.DestroyBuffer(buffer);

    EXPECT_EQ(descriptor.SizeBytes, 256U);
    EXPECT_EQ(descriptor.Usage, NGraphics::BufferUsage(NGraphics::EBufferUsage::Vertex));
    EXPECT_EQ(descriptor.Access, NGraphics::BufferAccess(NGraphics::EBufferAccess::GpuRead));
}

TEST_F(GraphicsTest, RejectsForeignCompletionForDeferredBufferDestruction) {
    NGraphics::Graphics graphics = CreateGraphics();

    FakeGraphicsState otherState;
    g_fakeGraphicsState = &otherState;
    NGraphics::Graphics other = CreateGraphics();
    g_fakeGraphicsState = &m_state;

    const NGraphics::CompletionPoint otherCompletion = other.SubmitFrame({
            .FrameIndex = 1,
            .RequiresPresentation = false,
    });

    const NGraphics::BufferHandle buffer = graphics.CreateBuffer({
            .SizeBytes = 8,
            .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::TransferSource),
            .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::CpuRead),
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { graphics.DestroyBuffer(buffer, otherCompletion); });
    EXPECT_EQ(m_state.DestroyedBuffers.size(), 0U);
}

TEST_F(GraphicsTest, GraphicsPipelineDescriptorEqualityAndHashAreStable) {
    const NGraphics::GraphicsPipelineDescriptor first = MakeGraphicsPipelineDescriptor();
    NGraphics::GraphicsPipelineDescriptor second = first;

    EXPECT_EQ(first, second);
    EXPECT_EQ(NGraphics::HashGraphicsPipelineDescriptor(first), NGraphics::HashGraphicsPipelineDescriptor(second));

    second.Topology = NGraphics::EPrimitiveTopology::LINE_LIST;

    EXPECT_NE(first, second);
    EXPECT_NE(NGraphics::HashGraphicsPipelineDescriptor(first), NGraphics::HashGraphicsPipelineDescriptor(second));
}

TEST_F(GraphicsTest, RejectsInvalidGraphicsPipelineDescriptors) {
    NGraphics::GraphicsPipelineDescriptor missingFragment = MakeGraphicsPipelineDescriptor();
    missingFragment.Shaders.pop_back();

    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { NGraphics::ValidateGraphicsPipelineDescriptor(missingFragment); });

    NGraphics::GraphicsPipelineDescriptor invalidVertexLayout = MakeGraphicsPipelineDescriptor();
    invalidVertexLayout.VertexLayout.Attributes[1].OffsetBytes = 16;

    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { NGraphics::ValidateGraphicsPipelineDescriptor(invalidVertexLayout); });

    NGraphics::GraphicsPipelineDescriptor invalidBlendState = MakeGraphicsPipelineDescriptor();
    invalidBlendState.ColorBlendAttachments.clear();

    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { NGraphics::ValidateGraphicsPipelineDescriptor(invalidBlendState); });

    NGraphics::GraphicsPipelineDescriptor invalidDepthState = MakeGraphicsPipelineDescriptor();
    invalidDepthState.DepthState.TestEnabled = true;

    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { NGraphics::ValidateGraphicsPipelineDescriptor(invalidDepthState); });

    NGraphics::GraphicsPipelineDescriptor invalidShader = MakeGraphicsPipelineDescriptor();
    invalidShader.Shaders[0] =
            NGraphics::Shader{NGraphics::EShaderStage::VERTEX, NGraphics::ShaderArtifact{.Words = {1U, 2U, 3U}}};

    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { NGraphics::ValidateGraphicsPipelineDescriptor(invalidShader); });
}

TEST_F(GraphicsTest, CreatesGraphicsPipelineAndPreservesDescriptor) {
    NGraphics::Graphics graphics = CreateGraphics();
    const NGraphics::GraphicsPipelineDescriptor descriptor = MakeGraphicsPipelineDescriptor();

    const NGraphics::GraphicsPipelineHandle pipeline = graphics.CreateGraphicsPipeline(descriptor);

    ASSERT_TRUE(pipeline.IsValid());
    ASSERT_EQ(m_state.CreatedGraphicsPipelines.size(), 1U);
    EXPECT_EQ(m_state.CreatedGraphicsPipelines[0], descriptor);
    EXPECT_EQ(graphics.GetGraphicsPipelineDescriptor(pipeline), descriptor);
}

TEST_F(GraphicsTest, DestroysGraphicsPipelineAfterCompletionPoint) {
    NGraphics::Graphics graphics = CreateGraphics();
    const NGraphics::GraphicsPipelineHandle pipeline = graphics.CreateGraphicsPipeline(MakeGraphicsPipelineDescriptor());
    const NGraphics::CompletionPoint completion = graphics.SubmitFrame({
            .FrameIndex = 11,
            .RequiresPresentation = false,
    });

    graphics.DestroyGraphicsPipeline(pipeline, completion);

    ASSERT_EQ(m_state.DestroyedGraphicsPipelines.size(), 1U);
    EXPECT_EQ(m_state.DestroyedGraphicsPipelines[0].first, pipeline.GetValue());
    ASSERT_TRUE(m_state.DestroyedGraphicsPipelines[0].second.has_value());
    EXPECT_EQ(*m_state.DestroyedGraphicsPipelines[0].second, completion.GetValue());
}

TEST_F(GraphicsTest, RejectsForeignAndStaleGraphicsPipelineHandles) {
    NGraphics::Graphics graphics = CreateGraphics();
    const NGraphics::GraphicsPipelineHandle pipeline = graphics.CreateGraphicsPipeline(MakeGraphicsPipelineDescriptor());

    FakeGraphicsState otherState;
    g_fakeGraphicsState = &otherState;
    NGraphics::Graphics other = CreateGraphics();
    g_fakeGraphicsState = &m_state;

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { other.DestroyGraphicsPipeline(pipeline); });

    graphics.DestroyGraphicsPipeline(pipeline);

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)graphics.GetGraphicsPipelineDescriptor(pipeline); });
}

TEST_F(GraphicsTest, RollsBackBackendPipelineWhenPublicationFails) {
    NGraphics::Graphics graphics = CreateGraphics();
    const NGraphics::GraphicsPipelineHandle first = graphics.CreateGraphicsPipeline(MakeGraphicsPipelineDescriptor());

    m_state.NextGraphicsPipeline = first.GetValue();

    ExpectError(NCommon::EError::INVALID_STATE,
                [&] { (void)graphics.CreateGraphicsPipeline(MakeGraphicsPipelineDescriptor()); });

    ASSERT_EQ(m_state.DestroyedGraphicsPipelines.size(), 1U);
    EXPECT_EQ(m_state.DestroyedGraphicsPipelines[0].first, first.GetValue());
    EXPECT_FALSE(m_state.DestroyedGraphicsPipelines[0].second.has_value());
    EXPECT_EQ(graphics.GetGraphicsPipelineDescriptor(first), MakeGraphicsPipelineDescriptor());
}

TEST_F(GraphicsTest, RejectsForeignCompletionForDeferredGraphicsPipelineDestruction) {
    NGraphics::Graphics graphics = CreateGraphics();
    const NGraphics::GraphicsPipelineHandle pipeline = graphics.CreateGraphicsPipeline(MakeGraphicsPipelineDescriptor());

    FakeGraphicsState otherState;
    g_fakeGraphicsState = &otherState;
    NGraphics::Graphics other = CreateGraphics();
    g_fakeGraphicsState = &m_state;

    const NGraphics::CompletionPoint otherCompletion = other.SubmitFrame({
            .FrameIndex = 1,
            .RequiresPresentation = false,
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { graphics.DestroyGraphicsPipeline(pipeline, otherCompletion); });
    EXPECT_EQ(m_state.DestroyedGraphicsPipelines.size(), 0U);
}

} // namespace
