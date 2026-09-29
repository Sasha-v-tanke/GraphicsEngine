#include <atomic>
#include <cstdint>
#include <limits>

#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <graphics/graphics.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NGraphics {

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
    if (!completion.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Completion point is invalid");
    }

    if (completion.m_ownerId != m_ownerId) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Completion point belongs to another Graphics");
    }

    return m_backend->IsCompleted(completion.m_value);
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
