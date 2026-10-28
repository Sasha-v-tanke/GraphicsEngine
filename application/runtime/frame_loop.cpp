#include "frame_loop.h"

#include <memory>
#include <utility>

#include <application/runtime/presentation_runtime.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NApplication::NRuntime {

class FrameLoop::EngineStopGuard final {
public:
    explicit EngineStopGuard(NEngine::Engine& engine) noexcept
        : m_engine(engine) {
    }

    ~EngineStopGuard() {
        m_engine.Stop();
    }

    EngineStopGuard(const EngineStopGuard&) = delete;
    EngineStopGuard& operator=(const EngineStopGuard&) = delete;

private:
    NEngine::Engine& m_engine;
};

FrameLoop::FrameLoop(const ApplicationConfig& config)
    : FrameLoop(config, std::make_unique<DefaultEngineFactory>()) {
}

FrameLoop::FrameLoop(const ApplicationConfig& config, std::unique_ptr<IEngineFactory> engineFactory)
    : m_engineFactory(std::move(engineFactory))
    , m_engine([this, &config] {
        if (m_engineFactory == nullptr) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Application engine factory is null");
        }

        return CreateEngine(*m_engineFactory,
                            NEngine::EngineConfig{
                                    .MaxActiveFrames = config.MaxActiveFrames,
                                    .WorkerCount = config.WorkerCount,
                            });
    }()) {
}

FrameLoop::FrameLoop(const ApplicationConfig& config, std::unique_ptr<NGraphics::Graphics> graphics) {
    auto runtime = std::make_unique<PresentationRuntime>(std::move(graphics));
    m_presentation = runtime.get();
    m_engine = NEngine::NRuntime::EngineFactory::Create(
            {.MaxActiveFrames = config.MaxActiveFrames, .WorkerCount = config.WorkerCount},
            std::move(runtime));
}

void FrameLoop::PumpPresentation(int width, int height) {
    if (m_presentation) {
        m_presentation->Pump(width, height);
    }
}

FrameDiagnostics FrameLoop::GetDiagnostics() const {
    auto diagnostics = m_presentation ? m_presentation->GetDiagnostics() : FrameDiagnostics{};
    const auto recycled = m_engine->GetGpuRecycledFrameCount();
    diagnostics.Recycled = recycled >= diagnostics.Abandoned ? recycled - diagnostics.Abandoned : 0;
    return diagnostics;
}

FrameLoop::~FrameLoop() {
    Stop();
}

void FrameLoop::Start() {
    if (m_presentation) {
        m_presentation->Start();
    }
    m_engine->Start();
    m_frameCheckpoint = EFrameCheckpoint::READY_FOR_USER_UPDATE;
}

void FrameLoop::Stop() noexcept {
    if (m_engine != nullptr) {
        m_engine->Stop();
    }
}

void FrameLoop::CheckError() const {
    if (const auto error = m_engine->GetLastError()) {
        GRAPHICS_ENGINE_THROW(error->Code, "{}", error->Message);
    }
}

void FrameLoop::Step(IFrameLoopCallbacks& callbacks) {
    CheckError();
    if (m_frameCheckpoint == EFrameCheckpoint::READY_FOR_USER_UPDATE) {
        if (!m_engine->CanAcceptUpdate()) {
            return;
        }

        callbacks.OnUpdate();
        m_frameCheckpoint = EFrameCheckpoint::WAITING_ENGINE_UPDATE;
    }

    if (m_frameCheckpoint == EFrameCheckpoint::WAITING_ENGINE_UPDATE) {
        EngineUpdateCheckpoint();

        if (m_frameCheckpoint == EFrameCheckpoint::WAITING_ENGINE_UPDATE) {
            return;
        }
    }

    if (m_frameCheckpoint == EFrameCheckpoint::READY_FOR_USER_DRAW) {
        callbacks.OnDraw();
        m_frameCheckpoint = EFrameCheckpoint::WAITING_ENGINE_DRAW;
    }

    if (m_frameCheckpoint == EFrameCheckpoint::WAITING_ENGINE_DRAW) {
        EngineDrawCheckpoint();
    }
}

void FrameLoop::EngineUpdateCheckpoint() {
    if (m_frameCheckpoint != EFrameCheckpoint::WAITING_ENGINE_UPDATE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application update checkpoint is out of order");
    }

    if (m_engine->Update()) {
        m_frameCheckpoint = EFrameCheckpoint::READY_FOR_USER_DRAW;
    }
}

void FrameLoop::EngineDrawCheckpoint() {
    if (m_frameCheckpoint != EFrameCheckpoint::WAITING_ENGINE_DRAW) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application draw checkpoint is out of order");
    }

    if (m_engine->Draw()) {
        m_frameCheckpoint = EFrameCheckpoint::READY_FOR_USER_UPDATE;
    }
}

} // namespace NApplication::NRuntime
