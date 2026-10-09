#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <graphics/graphics.h>
#include <graphics/material.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <resources/resource_manager.h>
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
    std::vector<std::pair<std::uint64_t, NGraphics::FrameSubmission>> InFlightSubmissions;
    std::vector<NGraphics::BufferDescriptor> CreatedBuffers;
    std::vector<std::pair<std::uint64_t, std::optional<std::uint64_t>>> DestroyedBuffers;
    std::vector<NGraphics::ImageDescriptor> CreatedImages;
    std::vector<std::pair<std::uint64_t, std::optional<std::uint64_t>>> DestroyedImages;
    std::vector<NGraphics::ImageViewDescriptor> CreatedImageViews;
    std::vector<std::pair<std::uint64_t, std::optional<std::uint64_t>>> DestroyedImageViews;
    std::vector<NGraphics::SamplerDescriptor> CreatedSamplers;
    std::vector<std::pair<std::uint64_t, std::optional<std::uint64_t>>> DestroyedSamplers;
    std::uint64_t NextBuffer = 1;
    std::uint64_t NextImage = 1;
    std::uint64_t NextImageView = 1;
    std::uint64_t NextSampler = 1;
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

    [[nodiscard]] std::uint64_t SubmitFrame(NGraphics::FrameSubmission submission) override {
        const std::uint64_t completion = m_state.NextCompletion++;
        m_state.InFlightSubmissions.emplace_back(completion, std::move(submission));
        return completion;
    }

    [[nodiscard]] bool IsCompleted(std::uint64_t completionValue) const override {
        std::erase_if(m_state.InFlightSubmissions,
                      [&](const auto& entry) { return entry.first <= m_state.CompletedValue; });

        return completionValue <= m_state.CompletedValue;
    }

    [[nodiscard]] std::uint64_t CreateBuffer(const NGraphics::BufferDescriptor& descriptor) override {
        m_state.CreatedBuffers.push_back(descriptor);
        return m_state.NextBuffer++;
    }

    void DestroyBuffer(std::uint64_t bufferValue, std::optional<std::uint64_t> completedAfter) noexcept override {
        m_state.DestroyedBuffers.emplace_back(bufferValue, completedAfter);
    }

    [[nodiscard]] std::uint64_t CreateImage(const NGraphics::ImageDescriptor& descriptor) override {
        m_state.CreatedImages.push_back(descriptor);
        return m_state.NextImage++;
    }

    void DestroyImage(std::uint64_t imageValue, std::optional<std::uint64_t> completedAfter) noexcept override {
        m_state.DestroyedImages.emplace_back(imageValue, completedAfter);
    }

    [[nodiscard]] std::uint64_t CreateImageView(const NGraphics::ImageViewDescriptor& descriptor) override {
        m_state.CreatedImageViews.push_back(descriptor);
        return m_state.NextImageView++;
    }

    void DestroyImageView(std::uint64_t imageViewValue, std::optional<std::uint64_t> completedAfter) noexcept override {
        m_state.DestroyedImageViews.emplace_back(imageViewValue, completedAfter);
    }

    [[nodiscard]] std::uint64_t CreateSampler(const NGraphics::SamplerDescriptor& descriptor) override {
        m_state.CreatedSamplers.push_back(descriptor);
        return m_state.NextSampler++;
    }

    void DestroySampler(std::uint64_t samplerValue, std::optional<std::uint64_t> completedAfter) noexcept override {
        m_state.DestroyedSamplers.emplace_back(samplerValue, completedAfter);
    }

private:
    FakeGraphicsState& m_state;
};

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
    NResources::ResourceManager resources;
    const auto resource = resources.Request<int>(NResources::ResourceIdentity{"test", "retained"});
    const auto operation = resources.BeginLoading(resource);

    resources.PublishReady(operation, std::make_shared<int>(7));

    std::optional<NResources::ResourceLease<int>> lease = resources.TryAcquire(resource);

    ASSERT_TRUE(lease.has_value());

    const auto completion = graphics.SubmitFrame({
            .FrameIndex = 42,
            .RequiresPresentation = true,
            .ResourceUseRecords = {lease->GetUseRecord()},
    });

    ASSERT_TRUE(completion.IsValid());
    ASSERT_EQ(m_state.InFlightSubmissions.size(), 1U);
    EXPECT_EQ(m_state.InFlightSubmissions[0].second.FrameIndex, 42U);
    EXPECT_TRUE(m_state.InFlightSubmissions[0].second.RequiresPresentation);
    ASSERT_EQ(m_state.InFlightSubmissions[0].second.ResourceUseRecords.size(), 1U);
    EXPECT_TRUE(m_state.InFlightSubmissions[0].second.ResourceUseRecords[0].IsValid());
    EXPECT_EQ(m_state.InFlightSubmissions[0].second.ResourceUseRecords[0].GetVersion(), lease->GetVersion());
    EXPECT_FALSE(graphics.IsCompleted(completion));

    m_state.CompletedValue = completion.GetValue();

    EXPECT_TRUE(graphics.IsCompleted(completion));
}

TEST_F(GraphicsTest, RetainsSubmissionResourcesUntilCompletion) {
    NGraphics::Graphics graphics = CreateGraphics();
    NResources::ResourceManager resources;
    const auto resource = resources.Request<int>(NResources::ResourceIdentity{"test", "gpu-retained"});
    const auto operation = resources.BeginLoading(resource);
    auto payload = std::make_shared<int>(9);
    const std::weak_ptr<const int> weakPayload = payload;

    resources.PublishReady(operation, payload);
    payload.reset();

    std::optional<NResources::ResourceLease<int>> lease = resources.TryAcquire(resource);

    ASSERT_TRUE(lease.has_value());

    const auto completion = graphics.SubmitFrame({
            .FrameIndex = 43,
            .RequiresPresentation = false,
            .ResourceUseRecords = {lease->GetUseRecord()},
    });

    resources.RequestUnload(resource);
    resources.CompleteUnload(resource);
    lease.reset();

    ASSERT_FALSE(weakPayload.expired());
    ASSERT_EQ(m_state.InFlightSubmissions.size(), 1U);

    EXPECT_FALSE(graphics.IsCompleted(completion));
    EXPECT_FALSE(weakPayload.expired());

    m_state.CompletedValue = completion.GetValue();

    EXPECT_TRUE(graphics.IsCompleted(completion));
    EXPECT_TRUE(weakPayload.expired());
    EXPECT_TRUE(m_state.InFlightSubmissions.empty());
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
                .ResourceUseRecords = {},
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
            .ResourceUseRecords = {},
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
            .ResourceUseRecords = {},
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
            .ResourceUseRecords = {},
    });

    const NGraphics::BufferHandle buffer = graphics.CreateBuffer({
            .SizeBytes = 8,
            .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::TransferSource),
            .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::CpuRead),
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { graphics.DestroyBuffer(buffer, otherCompletion); });
    EXPECT_EQ(m_state.DestroyedBuffers.size(), 0U);
}

TEST_F(GraphicsTest, CreatesImageViewAndSamplerAndPreservesDescriptors) {
    NGraphics::Graphics graphics = CreateGraphics();

    const NGraphics::ImageDescriptor imageDescriptor{
            .Extent = {.Width = 128, .Height = 64, .Depth = 1},
            .MipLevels = 4,
            .ArrayLayers = 1,
            .Format = NGraphics::EImageFormat::RGBA8_UNORM,
            .Usage = NGraphics::EImageUsage::TransferDestination | NGraphics::EImageUsage::Sampled,
            .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
            .Lifetime = NGraphics::EImageLifetime::Persistent,
    };

    const NGraphics::ImageHandle image = graphics.CreateImage(imageDescriptor);
    const NGraphics::ImageViewDescriptor viewDescriptor{
            .Image = image,
            .Format = NGraphics::EImageFormat::RGBA8_UNORM,
            .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color),
            .BaseMipLevel = 1,
            .LevelCount = 2,
            .BaseArrayLayer = 0,
            .LayerCount = 1,
    };
    const NGraphics::SamplerDescriptor samplerDescriptor{
            .MinFilter = NGraphics::ESamplerFilter::Nearest,
            .MagFilter = NGraphics::ESamplerFilter::Linear,
            .AddressModeU = NGraphics::ESamplerAddressMode::Repeat,
            .AddressModeV = NGraphics::ESamplerAddressMode::ClampToEdge,
            .AddressModeW = NGraphics::ESamplerAddressMode::ClampToBorder,
            .MinLod = 0.0F,
            .MaxLod = 4.0F,
    };

    const NGraphics::ImageViewHandle view = graphics.CreateImageView(viewDescriptor);
    const NGraphics::SamplerHandle sampler = graphics.CreateSampler(samplerDescriptor);

    ASSERT_TRUE(image.IsValid());
    ASSERT_TRUE(view.IsValid());
    ASSERT_TRUE(sampler.IsValid());
    ASSERT_EQ(m_state.CreatedImages.size(), 1U);
    ASSERT_EQ(m_state.CreatedImageViews.size(), 1U);
    ASSERT_EQ(m_state.CreatedSamplers.size(), 1U);
    EXPECT_EQ(m_state.CreatedImages[0].Extent.Width, 128U);
    EXPECT_EQ(m_state.CreatedImages[0].MipLevels, 4U);
    EXPECT_EQ(m_state.CreatedImageViews[0].Image, image);
    EXPECT_EQ(m_state.CreatedSamplers[0].MaxLod, 4.0F);
    EXPECT_EQ(graphics.GetImageDescriptor(image).ArrayLayers, 1U);
    EXPECT_EQ(graphics.GetImageViewDescriptor(view).BaseMipLevel, 1U);
    EXPECT_EQ(graphics.GetSamplerDescriptor(sampler).AddressModeV, NGraphics::ESamplerAddressMode::ClampToEdge);
}

TEST_F(GraphicsTest, RejectsInvalidImageDescriptors) {
    NGraphics::Graphics graphics = CreateGraphics();

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImage({
                .Extent = {.Width = 0, .Height = 1, .Depth = 1},
                .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled),
                .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImage({
                .Extent = {.Width = 1, .Height = 1, .Depth = 1},
                .MipLevels = 0,
                .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled),
                .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImage({
                .Extent = {.Width = 1, .Height = 1, .Depth = 1},
                .MipLevels = 2,
                .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled),
                .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
        });
    });

    (void)graphics.CreateImage({
            .Extent = {.Width = 4, .Height = 4, .Depth = 1},
            .MipLevels = 3,
            .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled),
            .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImage({
                .Extent = {.Width = 4, .Height = 4, .Depth = 1},
                .MipLevels = 4,
                .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled),
                .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImage({
                .Extent = {.Width = 1, .Height = 1, .Depth = 2},
                .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled),
                .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImage({
                .Extent = {.Width = 1, .Height = 1, .Depth = 1},
                .ArrayLayers = 2,
                .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled),
                .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImage({
                .Extent = {.Width = 1, .Height = 1, .Depth = 1},
                .Usage = 0,
                .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImage({
                .Extent = {.Width = 1, .Height = 1, .Depth = 1},
                .Usage = 0x80000000U,
                .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImage({
                .Extent = {.Width = 1, .Height = 1, .Depth = 1},
                .Format = NGraphics::EImageFormat::D32_FLOAT,
                .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::ColorAttachment),
                .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuWrite),
        });
    });
}

TEST_F(GraphicsTest, RejectsInvalidImageViewBindings) {
    NGraphics::Graphics graphics = CreateGraphics();

    const NGraphics::ImageHandle image = graphics.CreateImage({
            .Extent = {.Width = 4, .Height = 4, .Depth = 1},
            .MipLevels = 2,
            .ArrayLayers = 1,
            .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled),
            .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImageView({
                .Image = image,
                .Format = NGraphics::EImageFormat::RGBA8_UNORM,
                .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Depth),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImageView({
                .Image = image,
                .Format = NGraphics::EImageFormat::BGRA8_UNORM,
                .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color),
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImageView({
                .Image = image,
                .Format = NGraphics::EImageFormat::RGBA8_UNORM,
                .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color),
                .BaseMipLevel = 1,
                .LevelCount = 2,
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateImageView({
                .Image = image,
                .Format = NGraphics::EImageFormat::RGBA8_UNORM,
                .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color),
                .LayerCount = 2,
        });
    });
}

TEST_F(GraphicsTest, TracksImageResourceLifetimeAndRejectsStaleHandles) {
    NGraphics::Graphics graphics = CreateGraphics();

    const NGraphics::ImageHandle image = graphics.CreateImage({
            .Extent = {.Width = 8, .Height = 8, .Depth = 1},
            .Usage = NGraphics::EImageUsage::TransferDestination | NGraphics::EImageUsage::Sampled,
            .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
            .Lifetime = NGraphics::EImageLifetime::FrameLocal,
    });
    const NGraphics::ImageViewHandle view = graphics.CreateImageView({
            .Image = image,
            .Format = NGraphics::EImageFormat::RGBA8_UNORM,
            .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color),
    });
    const NGraphics::SamplerHandle sampler = graphics.CreateSampler({});
    const NGraphics::CompletionPoint completion = graphics.SubmitFrame({
            .FrameIndex = 11,
            .RequiresPresentation = false,
            .ResourceUseRecords = {},
    });

    graphics.DestroyImageView(view, completion);
    graphics.DestroySampler(sampler, completion);
    graphics.DestroyImage(image, completion);

    ASSERT_EQ(m_state.DestroyedImageViews.size(), 1U);
    ASSERT_EQ(m_state.DestroyedSamplers.size(), 1U);
    ASSERT_EQ(m_state.DestroyedImages.size(), 1U);
    EXPECT_EQ(*m_state.DestroyedImageViews[0].second, completion.GetValue());
    EXPECT_EQ(*m_state.DestroyedSamplers[0].second, completion.GetValue());
    EXPECT_EQ(*m_state.DestroyedImages[0].second, completion.GetValue());

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)graphics.GetImageViewDescriptor(view); });
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)graphics.GetSamplerDescriptor(sampler); });
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)graphics.GetImageDescriptor(image); });
}

TEST_F(GraphicsTest, RejectsForeignImageAndSamplerHandles) {
    NGraphics::Graphics graphics = CreateGraphics();

    const NGraphics::ImageHandle image = graphics.CreateImage({
            .Extent = {.Width = 8, .Height = 8, .Depth = 1},
            .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled),
            .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
    });
    const NGraphics::SamplerHandle sampler = graphics.CreateSampler({});

    FakeGraphicsState otherState;
    g_fakeGraphicsState = &otherState;
    NGraphics::Graphics other = CreateGraphics();
    g_fakeGraphicsState = &m_state;

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { other.DestroyImage(image); });
    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { other.DestroySampler(sampler); });
}

TEST_F(GraphicsTest, RejectsDestroyingImageWithLiveViews) {
    NGraphics::Graphics graphics = CreateGraphics();

    const NGraphics::ImageHandle image = graphics.CreateImage({
            .Extent = {.Width = 8, .Height = 8, .Depth = 1},
            .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled),
            .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
    });
    const NGraphics::ImageViewHandle view = graphics.CreateImageView({
            .Image = image,
            .Format = NGraphics::EImageFormat::RGBA8_UNORM,
            .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color),
    });

    ExpectError(NCommon::EError::INVALID_STATE, [&] { graphics.DestroyImage(image); });

    graphics.DestroyImageView(view);
    graphics.DestroyImage(image);

    EXPECT_EQ(m_state.DestroyedImageViews.size(), 1U);
    EXPECT_EQ(m_state.DestroyedImages.size(), 1U);
}

TEST_F(GraphicsTest, RejectsInvalidSamplerDescriptor) {
    NGraphics::Graphics graphics = CreateGraphics();

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateSampler({
                .MinLod = 2.0F,
                .MaxLod = 1.0F,
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateSampler({
                .MinLod = std::numeric_limits<float>::quiet_NaN(),
                .MaxLod = 1.0F,
        });
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        (void)graphics.CreateSampler({
                .MinLod = 0.0F,
                .MaxLod = std::numeric_limits<float>::infinity(),
        });
    });
}

TEST_F(GraphicsTest, CreatesMaterialAndPreservesBindingDescriptors) {
    const NGraphics::Material material{
            NResources::ResourceIdentity{"pipeline", "textured"},
            {
                    {
                            .Binding = 0,
                            .Type = NGraphics::EMaterialBindingType::UniformBuffer,
                            .Visibility = NGraphics::ShaderVisibility(NGraphics::EShaderVisibility::Vertex),
                    },
                    {
                            .Binding = 1,
                            .Type = NGraphics::EMaterialBindingType::CombinedImageSampler,
                            .Visibility = NGraphics::ShaderVisibility(NGraphics::EShaderVisibility::Fragment),
                    },
            },
            {
                    {
                            .Binding = 0,
                            .Type = NGraphics::EMaterialBindingType::UniformBuffer,
                            .Buffer =
                                    {
                                            .Resource = NResources::ResourceIdentity{"buffer", "camera"},
                                            .SizeBytes = 256,
                                    },
                            .Image = {},
                            .Sampler = {},
                            .CombinedImageSampler = {},
                    },
                    {
                            .Binding = 1,
                            .Type = NGraphics::EMaterialBindingType::CombinedImageSampler,
                            .Buffer = {},
                            .Image = {},
                            .Sampler = {},
                            .CombinedImageSampler =
                                    {
                                            .Image = NResources::ResourceIdentity{"image", "albedo"},
                                            .Sampler = NResources::ResourceIdentity{"sampler", "linear"},
                                    },
                    },
            },
    };

    EXPECT_EQ(material.GetPipeline(), (NResources::ResourceIdentity{"pipeline", "textured"}));
    ASSERT_EQ(material.GetLayout().size(), 2U);
    EXPECT_EQ(material.GetLayout()[1].Binding, 1U);
    EXPECT_EQ(material.GetLayout()[1].Type, NGraphics::EMaterialBindingType::CombinedImageSampler);
    ASSERT_EQ(material.GetBindings().size(), 2U);
    EXPECT_EQ(material.GetBindings()[0].Buffer.Resource, (NResources::ResourceIdentity{"buffer", "camera"}));
    EXPECT_EQ(material.GetBindings()[0].Buffer.SizeBytes, 256U);
    EXPECT_EQ(material.GetBindings()[1].CombinedImageSampler.Image, (NResources::ResourceIdentity{"image", "albedo"}));
    EXPECT_EQ(material.GetBindings()[1].CombinedImageSampler.Sampler,
              (NResources::ResourceIdentity{"sampler", "linear"}));
}

TEST_F(GraphicsTest, RejectsInvalidMaterialDescriptors) {
    const std::vector<NGraphics::MaterialBindingLayoutEntry> layout{
            {
                    .Binding = 0,
                    .Type = NGraphics::EMaterialBindingType::UniformBuffer,
                    .Visibility = NGraphics::ShaderVisibility(NGraphics::EShaderVisibility::Vertex),
            },
    };
    const std::vector<NGraphics::MaterialBinding> bindings{
            {
                    .Binding = 0,
                    .Type = NGraphics::EMaterialBindingType::UniformBuffer,
                    .Buffer =
                            {
                                    .Resource = NResources::ResourceIdentity{"buffer", "camera"},
                                    .SizeBytes = 64,
                            },
                    .Image = {},
                    .Sampler = {},
                    .CombinedImageSampler = {},
            },
    };

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { const NGraphics::Material material{{}, layout, bindings}; });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        const NGraphics::Material material{NResources::ResourceIdentity{"shader", "wrong"}, layout, bindings};
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        const NGraphics::Material material{
                NResources::ResourceIdentity{"pipeline", "duplicate-layout"},
                {layout[0], layout[0]},
                bindings,
        };
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        const NGraphics::Material material{
                NResources::ResourceIdentity{"pipeline", "array"},
                {
                        {
                                .Binding = 0,
                                .Type = NGraphics::EMaterialBindingType::UniformBuffer,
                                .Visibility = NGraphics::ShaderVisibility(NGraphics::EShaderVisibility::Vertex),
                                .Count = 2,
                        },
                },
                bindings,
        };
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        const NGraphics::Material material{
                NResources::ResourceIdentity{"pipeline", "missing"},
                layout,
                {},
        };
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        const NGraphics::Material material{
                NResources::ResourceIdentity{"pipeline", "type-mismatch"},
                layout,
                {
                        {
                                .Binding = 0,
                                .Type = NGraphics::EMaterialBindingType::Sampler,
                                .Buffer = {},
                                .Image = {},
                                .Sampler = {},
                                .CombinedImageSampler = {},
                        },
                },
        };
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        const NGraphics::Material material{
                NResources::ResourceIdentity{"pipeline", "invalid-buffer"},
                layout,
                {
                        {
                                .Binding = 0,
                                .Type = NGraphics::EMaterialBindingType::UniformBuffer,
                                .Buffer =
                                        {
                                                .Resource = NResources::ResourceIdentity{"buffer", "camera"},
                                                .SizeBytes = 0,
                                        },
                                .Image = {},
                                .Sampler = {},
                                .CombinedImageSampler = {},
                        },
                },
        };
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        const NGraphics::Material material{
                NResources::ResourceIdentity{"pipeline", "wrong-resource-class"},
                layout,
                {
                        {
                                .Binding = 0,
                                .Type = NGraphics::EMaterialBindingType::UniformBuffer,
                                .Buffer =
                                        {
                                                .Resource = NResources::ResourceIdentity{"image", "camera"},
                                                .SizeBytes = 64,
                                        },
                                .Image = {},
                                .Sampler = {},
                                .CombinedImageSampler = {},
                        },
                },
        };
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        const NGraphics::Material material{
                NResources::ResourceIdentity{"pipeline", "range-overflow"},
                layout,
                {
                        {
                                .Binding = 0,
                                .Type = NGraphics::EMaterialBindingType::UniformBuffer,
                                .Buffer =
                                        {
                                                .Resource = NResources::ResourceIdentity{"buffer", "camera"},
                                                .OffsetBytes = std::numeric_limits<std::uint64_t>::max(),
                                                .SizeBytes = 1,
                                        },
                                .Image = {},
                                .Sampler = {},
                                .CombinedImageSampler = {},
                        },
                },
        };
    });
}

} // namespace
