#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <graphics/graphics.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NGraphics {

Graphics::Graphics(const GraphicsConfig& config)
    : m_backend(NBackend::CreateGraphicsBackend(config)) {
    if (!SatisfiesRequirements(m_backend->GetCapabilities(), config.RequiredCapabilities)) {
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

    return m_backend->SubmitFrame(submission);
}

bool Graphics::IsCompleted(CompletionPoint completion) const {
    if (!completion.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Completion point is invalid");
    }

    return m_backend->IsCompleted(completion);
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
