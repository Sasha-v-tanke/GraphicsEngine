#include "frame_loop.h"

#include <memory>
#include <utility>

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

FrameLoop::~FrameLoop() {
    Stop();
}

void FrameLoop::Start() {
    m_engine->Start();
    m_frameCheckpoint = EFrameCheckpoint::READY_FOR_USER_UPDATE;
}

void FrameLoop::Stop() noexcept {
    if (m_engine != nullptr) {
        m_engine->Stop();
    }
}

void FrameLoop::Step(IFrameLoopCallbacks& callbacks) {
    if (m_frameCheckpoint == EFrameCheckpoint::READY_FOR_USER_UPDATE) {
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
