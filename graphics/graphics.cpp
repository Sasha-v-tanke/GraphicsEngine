#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>

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

class BufferCreationGuard final {
public:
    BufferCreationGuard(NBackend::IGraphicsBackend& backend, std::uint64_t value) noexcept
        : m_backend(backend)
        , m_value(value) {
    }

    ~BufferCreationGuard() noexcept {
        if (m_active) {
            try {
                m_backend.DestroyBuffer(m_value, std::nullopt);
            } catch (...) {
            }
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

CompletionPoint Graphics::SubmitFrame(const FrameSubmission& submission) {
    if (submission.RequiresPresentation && !m_backend->GetCapabilities().Presentation) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Graphics backend does not support presentation");
    }

    const std::uint64_t value = m_backend->SubmitFrame(submission);

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
