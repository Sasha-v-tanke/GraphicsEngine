#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <graphics/buffer.h>
#include <graphics/completion_point.h>
#include <graphics/frame_submission.h>
#include <graphics/graphics_capabilities.h>
#include <graphics/graphics_config.h>
#include <graphics/graphics_pipeline.h>
#include <graphics/image.h>
#include <lib/common/wrapper/non_transferable.h>

namespace NGraphics {

class Graphics;

namespace NBackend {

class IGraphicsBackend;
[[nodiscard]] Graphics CreateGraphicsForBackend(std::unique_ptr<IGraphicsBackend> backend,
                                                const RequiredGraphicsCapabilities& requiredCapabilities);

} // namespace NBackend

class Graphics final: public NCommon::NonTransferable {
public:
    explicit Graphics(const GraphicsConfig& config = {});
    ~Graphics();

    [[nodiscard]] const GraphicsCapabilities& GetCapabilities() const noexcept;

    [[nodiscard]] CompletionPoint SubmitFrame(FrameSubmission submission);
    // Nonblocking acquire; retries retain the same frame identity. Inspect Completion before Failure.
    [[nodiscard]] FrameSubmissionResult TrySubmitFrame(FrameSubmission submission);
    void SetPresentationSize(int width, int height);
    [[nodiscard]] bool IsCompleted(CompletionPoint completion) const;

    [[nodiscard]] BufferHandle CreateBuffer(const BufferDescriptor& descriptor);
    void DestroyBuffer(BufferHandle buffer, CompletionPoint completedAfter = {});
    [[nodiscard]] BufferDescriptor GetBufferDescriptor(BufferHandle buffer) const;
    [[nodiscard]] ImageHandle CreateImage(const ImageDescriptor& descriptor);
    void DestroyImage(ImageHandle image, CompletionPoint completedAfter = {});
    [[nodiscard]] ImageDescriptor GetImageDescriptor(ImageHandle image) const;
    [[nodiscard]] ImageViewHandle CreateImageView(const ImageViewDescriptor& descriptor);
    void DestroyImageView(ImageViewHandle imageView, CompletionPoint completedAfter = {});
    [[nodiscard]] ImageViewDescriptor GetImageViewDescriptor(ImageViewHandle imageView) const;
    [[nodiscard]] SamplerHandle CreateSampler(const SamplerDescriptor& descriptor);
    void DestroySampler(SamplerHandle sampler, CompletionPoint completedAfter = {});
    [[nodiscard]] SamplerDescriptor GetSamplerDescriptor(SamplerHandle sampler) const;

    [[nodiscard]] GraphicsPipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDescriptor& descriptor);
    void DestroyGraphicsPipeline(GraphicsPipelineHandle pipeline, CompletionPoint completedAfter = {});
    [[nodiscard]] GraphicsPipelineDescriptor GetGraphicsPipelineDescriptor(GraphicsPipelineHandle pipeline) const;

private:
    explicit Graphics(std::unique_ptr<NBackend::IGraphicsBackend> backend,
                      const RequiredGraphicsCapabilities& requiredCapabilities = {});

    friend class NBackend::CompletionAccess;
    [[nodiscard]] static std::uint64_t AcquireOwnerId();

    struct BufferRecord {
        BufferDescriptor Descriptor;
        std::uint64_t Generation = 0;
    };

    struct GraphicsPipelineRecord {
        GraphicsPipelineDescriptor Descriptor;
        std::uint64_t Generation = 0;
    };

    struct ImageRecord {
        ImageDescriptor Descriptor;
        std::uint64_t Generation = 0;
    };

    struct ImageViewRecord {
        ImageViewDescriptor Descriptor;
        std::uint64_t Generation = 0;
    };

    struct SamplerRecord {
        SamplerDescriptor Descriptor;
        std::uint64_t Generation = 0;
    };

    [[nodiscard]] const BufferRecord& ResolveBuffer(BufferHandle buffer) const;
    [[nodiscard]] const ImageRecord& ResolveImage(ImageHandle image) const;
    [[nodiscard]] const ImageViewRecord& ResolveImageView(ImageViewHandle imageView) const;
    [[nodiscard]] const SamplerRecord& ResolveSampler(SamplerHandle sampler) const;
    [[nodiscard]] const GraphicsPipelineRecord& ResolveGraphicsPipeline(GraphicsPipelineHandle pipeline) const;
    void ValidateCompletionOwner(CompletionPoint completion) const;

    std::unique_ptr<NBackend::IGraphicsBackend> m_backend;
    std::uint64_t m_ownerId = 0;
    std::uint64_t m_nextBufferGeneration = 1;
    std::uint64_t m_nextGraphicsPipelineGeneration = 1;
    std::unordered_map<std::uint64_t, GraphicsPipelineRecord> m_graphicsPipelines;
    std::uint64_t m_nextImageGeneration = 1;
    std::uint64_t m_nextImageViewGeneration = 1;
    std::uint64_t m_nextSamplerGeneration = 1;
    std::unordered_map<std::uint64_t, BufferRecord> m_buffers;
    std::unordered_map<std::uint64_t, ImageRecord> m_images;
    std::unordered_map<std::uint64_t, ImageViewRecord> m_imageViews;
    std::unordered_map<std::uint64_t, SamplerRecord> m_samplers;

    friend Graphics NBackend::CreateGraphicsForBackend(std::unique_ptr<NBackend::IGraphicsBackend> backend,
                                                       const RequiredGraphicsCapabilities& requiredCapabilities);
};

} // namespace NGraphics
