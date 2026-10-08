#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <graphics/graphics.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NGraphics {

namespace {

constexpr BufferUsageFlags KNOWN_BUFFER_USAGE_MASK =
        BufferUsage(EBufferUsage::TransferSource) | BufferUsage(EBufferUsage::TransferDestination) |
        BufferUsage(EBufferUsage::Vertex) | BufferUsage(EBufferUsage::Index) | BufferUsage(EBufferUsage::Uniform) |
        BufferUsage(EBufferUsage::Storage);

constexpr BufferAccessFlags KNOWN_BUFFER_ACCESS_MASK =
        BufferAccess(EBufferAccess::CpuRead) | BufferAccess(EBufferAccess::CpuWrite) |
        BufferAccess(EBufferAccess::GpuRead) | BufferAccess(EBufferAccess::GpuWrite);

constexpr ImageUsageFlags KNOWN_IMAGE_USAGE_MASK =
        ImageUsage(EImageUsage::TransferSource) | ImageUsage(EImageUsage::TransferDestination) |
        ImageUsage(EImageUsage::Sampled) | ImageUsage(EImageUsage::ColorAttachment) |
        ImageUsage(EImageUsage::DepthStencilAttachment);

constexpr ImageAccessFlags KNOWN_IMAGE_ACCESS_MASK =
        ImageAccess(EImageAccess::GpuRead) | ImageAccess(EImageAccess::GpuWrite);

constexpr ImageAspectFlags KNOWN_IMAGE_ASPECT_MASK =
        ImageAspect(EImageAspect::Color) | ImageAspect(EImageAspect::Depth);

[[nodiscard]] bool IsDepthFormat(EImageFormat format) noexcept {
    return format == EImageFormat::D32_FLOAT;
}

[[nodiscard]] ImageAspectFlags ExpectedImageAspect(EImageFormat format) noexcept {
    if (IsDepthFormat(format)) {
        return ImageAspect(EImageAspect::Depth);
    }

    return ImageAspect(EImageAspect::Color);
}

[[nodiscard]] std::uint32_t CalculateMaxMipLevels(const ImageExtent& extent) noexcept {
    std::uint32_t maxDimension = extent.Width;

    if (extent.Height > maxDimension) {
        maxDimension = extent.Height;
    }

    if (extent.Depth > maxDimension) {
        maxDimension = extent.Depth;
    }

    std::uint32_t levels = 1;

    while (maxDimension > 1) {
        maxDimension /= 2;
        ++levels;
    }

    return levels;
}

class BufferCreationGuard final {
public:
    BufferCreationGuard(NBackend::IGraphicsBackend& backend, std::uint64_t value) noexcept
        : m_backend(backend)
        , m_value(value) {
    }

    ~BufferCreationGuard() noexcept {
        if (m_active) {
            m_backend.DestroyBuffer(m_value, std::nullopt);
        }
    }

    void Release() noexcept {
        m_active = false;
    }

private:
    NBackend::IGraphicsBackend& m_backend;
    std::uint64_t m_value = 0;
    bool m_active = true;
};

class ImageCreationGuard final {
public:
    ImageCreationGuard(NBackend::IGraphicsBackend& backend, std::uint64_t value) noexcept
        : m_backend(backend)
        , m_value(value) {
    }

    ~ImageCreationGuard() noexcept {
        if (m_active) {
            m_backend.DestroyImage(m_value, std::nullopt);
        }
    }

    void Release() noexcept {
        m_active = false;
    }

private:
    NBackend::IGraphicsBackend& m_backend;
    std::uint64_t m_value = 0;
    bool m_active = true;
};

class ImageViewCreationGuard final {
public:
    ImageViewCreationGuard(NBackend::IGraphicsBackend& backend, std::uint64_t value) noexcept
        : m_backend(backend)
        , m_value(value) {
    }

    ~ImageViewCreationGuard() noexcept {
        if (m_active) {
            m_backend.DestroyImageView(m_value, std::nullopt);
        }
    }

    void Release() noexcept {
        m_active = false;
    }

private:
    NBackend::IGraphicsBackend& m_backend;
    std::uint64_t m_value = 0;
    bool m_active = true;
};

class SamplerCreationGuard final {
public:
    SamplerCreationGuard(NBackend::IGraphicsBackend& backend, std::uint64_t value) noexcept
        : m_backend(backend)
        , m_value(value) {
    }

    ~SamplerCreationGuard() noexcept {
        if (m_active) {
            m_backend.DestroySampler(m_value, std::nullopt);
        }
    }

    void Release() noexcept {
        m_active = false;
    }

private:
    NBackend::IGraphicsBackend& m_backend;
    std::uint64_t m_value = 0;
    bool m_active = true;
};

} // namespace

Graphics::Graphics(const GraphicsConfig& config)
    : Graphics(NBackend::CreateGraphicsBackend(config), config.RequiredCapabilities) {
}

Graphics::Graphics(std::unique_ptr<NBackend::IGraphicsBackend> backend,
                   const RequiredGraphicsCapabilities& requiredCapabilities)
    : m_backend(std::move(backend))
    , m_ownerId(AcquireOwnerId()) {
    if (m_backend == nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graphics backend is null");
    }

    if (!SatisfiesRequirements(m_backend->GetCapabilities(), requiredCapabilities)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Graphics backend does not satisfy required capabilities");
    }
}

Graphics::~Graphics() = default;

const GraphicsCapabilities& Graphics::GetCapabilities() const noexcept {
    return m_backend->GetCapabilities();
}

CompletionPoint Graphics::SubmitFrame(FrameSubmission submission) {
    if (submission.RequiresPresentation && !m_backend->GetCapabilities().Presentation) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Graphics backend does not support presentation");
    }

    const std::uint64_t value = m_backend->SubmitFrame(std::move(submission));

    if (value == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graphics backend returned invalid completion value");
    }

    return CompletionPoint{m_ownerId, value};
}

bool Graphics::IsCompleted(CompletionPoint completion) const {
    ValidateCompletionOwner(completion);
    return m_backend->IsCompleted(completion.m_value);
}

BufferHandle Graphics::CreateBuffer(const BufferDescriptor& descriptor) {
    if (descriptor.SizeBytes == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Buffer size must be greater than zero");
    }

    if (descriptor.Usage == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Buffer usage must not be empty");
    }

    if ((descriptor.Usage & ~KNOWN_BUFFER_USAGE_MASK) != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Buffer usage contains unknown flags");
    }

    if (descriptor.Access == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Buffer access must not be empty");
    }

    if ((descriptor.Access & ~KNOWN_BUFFER_ACCESS_MASK) != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Buffer access contains unknown flags");
    }

    if (m_nextBufferGeneration == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::OUT_OF_MEMORY, "Buffer generation space is exhausted");
    }

    const std::uint64_t value = m_backend->CreateBuffer(descriptor);

    if (value == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graphics backend returned invalid buffer handle");
    }

    BufferCreationGuard creationGuard{*m_backend, value};

    const std::uint64_t generation = m_nextBufferGeneration;
    const auto [it, inserted] = m_buffers.emplace(value,
                                                  BufferRecord{
                                                          .Descriptor = descriptor,
                                                          .Generation = generation,
                                                  });

    if (!inserted) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graphics backend returned duplicate buffer handle");
    }

    ++m_nextBufferGeneration;
    creationGuard.Release();

    return BufferHandle{m_ownerId, value, it->second.Generation};
}

void Graphics::DestroyBuffer(BufferHandle buffer, CompletionPoint completedAfter) {
    const BufferRecord& record = ResolveBuffer(buffer);
    (void)record;

    std::optional<std::uint64_t> completedAfterValue;

    if (completedAfter.IsValid()) {
        ValidateCompletionOwner(completedAfter);
        completedAfterValue = completedAfter.m_value;
    }

    m_backend->DestroyBuffer(buffer.m_value, completedAfterValue);
    m_buffers.erase(buffer.m_value);
}

BufferDescriptor Graphics::GetBufferDescriptor(BufferHandle buffer) const {
    return ResolveBuffer(buffer).Descriptor;
}

ImageHandle Graphics::CreateImage(const ImageDescriptor& descriptor) {
    if (descriptor.Extent.Width == 0 || descriptor.Extent.Height == 0 || descriptor.Extent.Depth == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image extent must be greater than zero");
    }

    if (descriptor.Extent.Depth != 1) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Only 2D images are supported");
    }

    if (descriptor.MipLevels == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image mip level count must be greater than zero");
    }

    if (descriptor.MipLevels > CalculateMaxMipLevels(descriptor.Extent)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image mip level count exceeds full mip chain");
    }

    if (descriptor.ArrayLayers == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image array layer count must be greater than zero");
    }

    if (descriptor.ArrayLayers != 1) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Only single-layer 2D images are supported");
    }

    if (descriptor.Usage == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image usage must not be empty");
    }

    if ((descriptor.Usage & ~KNOWN_IMAGE_USAGE_MASK) != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image usage contains unknown flags");
    }

    if (descriptor.Access == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image access must not be empty");
    }

    if ((descriptor.Access & ~KNOWN_IMAGE_ACCESS_MASK) != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image access contains unknown flags");
    }

    if (IsDepthFormat(descriptor.Format) && (descriptor.Usage & ImageUsage(EImageUsage::ColorAttachment)) != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Depth image cannot use color attachment usage");
    }

    if (!IsDepthFormat(descriptor.Format) &&
        (descriptor.Usage & ImageUsage(EImageUsage::DepthStencilAttachment)) != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Color image cannot use depth attachment usage");
    }

    if (m_nextImageGeneration == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::OUT_OF_MEMORY, "Image generation space is exhausted");
    }

    const std::uint64_t value = m_backend->CreateImage(descriptor);

    if (value == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graphics backend returned invalid image handle");
    }

    ImageCreationGuard creationGuard{*m_backend, value};

    const std::uint64_t generation = m_nextImageGeneration;
    const auto [it, inserted] = m_images.emplace(value,
                                                 ImageRecord{
                                                         .Descriptor = descriptor,
                                                         .Generation = generation,
                                                 });

    if (!inserted) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graphics backend returned duplicate image handle");
    }

    ++m_nextImageGeneration;
    creationGuard.Release();

    return ImageHandle{m_ownerId, value, it->second.Generation};
}

void Graphics::DestroyImage(ImageHandle image, CompletionPoint completedAfter) {
    const ImageRecord& record = ResolveImage(image);
    (void)record;

    for (const auto& [_, imageView]: m_imageViews) {
        if (imageView.Descriptor.Image == image) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Image still has live image views");
        }
    }

    std::optional<std::uint64_t> completedAfterValue;

    if (completedAfter.IsValid()) {
        ValidateCompletionOwner(completedAfter);
        completedAfterValue = completedAfter.m_value;
    }

    m_backend->DestroyImage(image.m_value, completedAfterValue);
    m_images.erase(image.m_value);
}

ImageDescriptor Graphics::GetImageDescriptor(ImageHandle image) const {
    return ResolveImage(image).Descriptor;
}

ImageViewHandle Graphics::CreateImageView(const ImageViewDescriptor& descriptor) {
    const ImageRecord& image = ResolveImage(descriptor.Image);

    if (descriptor.Aspects == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image view aspect mask must not be empty");
    }

    if ((descriptor.Aspects & ~KNOWN_IMAGE_ASPECT_MASK) != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image view aspect mask contains unknown flags");
    }

    if (descriptor.Aspects != ExpectedImageAspect(image.Descriptor.Format)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Image view aspects are incompatible with image format");
    }

    if (descriptor.Format != image.Descriptor.Format) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image view format must match image format");
    }

    if (descriptor.LevelCount == 0 || descriptor.BaseMipLevel >= image.Descriptor.MipLevels ||
        descriptor.LevelCount > image.Descriptor.MipLevels - descriptor.BaseMipLevel) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image view mip range is invalid");
    }

    if (descriptor.LayerCount == 0 || descriptor.BaseArrayLayer >= image.Descriptor.ArrayLayers ||
        descriptor.LayerCount > image.Descriptor.ArrayLayers - descriptor.BaseArrayLayer) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image view layer range is invalid");
    }

    if (descriptor.BaseArrayLayer != 0 || descriptor.LayerCount != 1) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Only single-layer 2D image views are supported");
    }

    if (m_nextImageViewGeneration == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::OUT_OF_MEMORY, "Image view generation space is exhausted");
    }

    const std::uint64_t value = m_backend->CreateImageView(descriptor);

    if (value == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graphics backend returned invalid image view handle");
    }

    ImageViewCreationGuard creationGuard{*m_backend, value};

    const std::uint64_t generation = m_nextImageViewGeneration;
    const auto [it, inserted] = m_imageViews.emplace(value,
                                                     ImageViewRecord{
                                                             .Descriptor = descriptor,
                                                             .Generation = generation,
                                                     });

    if (!inserted) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graphics backend returned duplicate image view handle");
    }

    ++m_nextImageViewGeneration;
    creationGuard.Release();

    return ImageViewHandle{m_ownerId, value, it->second.Generation};
}

void Graphics::DestroyImageView(ImageViewHandle imageView, CompletionPoint completedAfter) {
    const ImageViewRecord& record = ResolveImageView(imageView);
    (void)record;

    std::optional<std::uint64_t> completedAfterValue;

    if (completedAfter.IsValid()) {
        ValidateCompletionOwner(completedAfter);
        completedAfterValue = completedAfter.m_value;
    }

    m_backend->DestroyImageView(imageView.m_value, completedAfterValue);
    m_imageViews.erase(imageView.m_value);
}

ImageViewDescriptor Graphics::GetImageViewDescriptor(ImageViewHandle imageView) const {
    return ResolveImageView(imageView).Descriptor;
}

SamplerHandle Graphics::CreateSampler(const SamplerDescriptor& descriptor) {
    if (!std::isfinite(descriptor.MinLod) || !std::isfinite(descriptor.MaxLod) || descriptor.MinLod < 0.0F ||
        descriptor.MaxLod < descriptor.MinLod) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Sampler LOD range is invalid");
    }

    if (m_nextSamplerGeneration == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::OUT_OF_MEMORY, "Sampler generation space is exhausted");
    }

    const std::uint64_t value = m_backend->CreateSampler(descriptor);

    if (value == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graphics backend returned invalid sampler handle");
    }

    SamplerCreationGuard creationGuard{*m_backend, value};

    const std::uint64_t generation = m_nextSamplerGeneration;
    const auto [it, inserted] = m_samplers.emplace(value,
                                                   SamplerRecord{
                                                           .Descriptor = descriptor,
                                                           .Generation = generation,
                                                   });

    if (!inserted) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graphics backend returned duplicate sampler handle");
    }

    ++m_nextSamplerGeneration;
    creationGuard.Release();

    return SamplerHandle{m_ownerId, value, it->second.Generation};
}

void Graphics::DestroySampler(SamplerHandle sampler, CompletionPoint completedAfter) {
    const SamplerRecord& record = ResolveSampler(sampler);
    (void)record;

    std::optional<std::uint64_t> completedAfterValue;

    if (completedAfter.IsValid()) {
        ValidateCompletionOwner(completedAfter);
        completedAfterValue = completedAfter.m_value;
    }

    m_backend->DestroySampler(sampler.m_value, completedAfterValue);
    m_samplers.erase(sampler.m_value);
}

SamplerDescriptor Graphics::GetSamplerDescriptor(SamplerHandle sampler) const {
    return ResolveSampler(sampler).Descriptor;
}

const Graphics::BufferRecord& Graphics::ResolveBuffer(BufferHandle buffer) const {
    if (!buffer.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Buffer handle is invalid");
    }

    if (buffer.m_ownerId != m_ownerId) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Buffer handle belongs to another Graphics");
    }

    const auto it = m_buffers.find(buffer.m_value);

    if (it == m_buffers.end() || it->second.Generation != buffer.m_generation) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Buffer handle is stale");
    }

    return it->second;
}

const Graphics::ImageRecord& Graphics::ResolveImage(ImageHandle image) const {
    if (!image.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image handle is invalid");
    }

    if (image.m_ownerId != m_ownerId) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image handle belongs to another Graphics");
    }

    const auto it = m_images.find(image.m_value);

    if (it == m_images.end() || it->second.Generation != image.m_generation) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image handle is stale");
    }

    return it->second;
}

const Graphics::ImageViewRecord& Graphics::ResolveImageView(ImageViewHandle imageView) const {
    if (!imageView.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image view handle is invalid");
    }

    if (imageView.m_ownerId != m_ownerId) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image view handle belongs to another Graphics");
    }

    const auto it = m_imageViews.find(imageView.m_value);

    if (it == m_imageViews.end() || it->second.Generation != imageView.m_generation) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Image view handle is stale");
    }

    return it->second;
}

const Graphics::SamplerRecord& Graphics::ResolveSampler(SamplerHandle sampler) const {
    if (!sampler.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Sampler handle is invalid");
    }

    if (sampler.m_ownerId != m_ownerId) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Sampler handle belongs to another Graphics");
    }

    const auto it = m_samplers.find(sampler.m_value);

    if (it == m_samplers.end() || it->second.Generation != sampler.m_generation) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Sampler handle is stale");
    }

    return it->second;
}

void Graphics::ValidateCompletionOwner(CompletionPoint completion) const {
    if (!completion.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Completion point is invalid");
    }

    if (completion.m_ownerId != m_ownerId) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Completion point belongs to another Graphics");
    }
}

std::uint64_t Graphics::AcquireOwnerId() {
    static std::atomic<std::uint64_t> nextOwnerId{1};

    std::uint64_t current = nextOwnerId.load(std::memory_order_relaxed);

    do {
        if (current == std::numeric_limits<std::uint64_t>::max()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::OUT_OF_MEMORY, "Graphics owner id space is exhausted");
        }
    } while (!nextOwnerId.compare_exchange_weak(current,
                                                current + 1,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed));

    return current;
}

bool SatisfiesRequirements(const GraphicsCapabilities& capabilities,
                           const RequiredGraphicsCapabilities& requirements) noexcept {
    if (requirements.Presentation && !capabilities.Presentation) {
        return false;
    }

    if (requirements.TimelineCompletion && !capabilities.TimelineCompletion) {
        return false;
    }

    return capabilities.MaxFramesInFlight >= requirements.MaxFramesInFlight;
}

} // namespace NGraphics
