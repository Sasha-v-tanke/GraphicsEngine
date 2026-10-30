#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <application/application_config.h>
#include <application/frame_diagnostics.h>
#include <application/runtime/engine_factory.h>
#include <application/runtime/main_executor.h>
#include <engine/engine.h>
#include <graphics/graphics.h>

namespace NApplication::NRuntime {

class PresentationRuntime;

class IFrameLoopCallbacks {
public:
    virtual ~IFrameLoopCallbacks() = default;

    virtual void OnUpdate() = 0;

    virtual void OnDraw() = 0;
    [[nodiscard]] virtual bool IsShutdownRequested() const {
        return false;
    }
};

class FrameLoop final: public NCommon::NonTransferable {
public:
    explicit FrameLoop(const ApplicationConfig& config);

    FrameLoop(const ApplicationConfig& config, std::unique_ptr<IEngineFactory> engineFactory);

    FrameLoop(const ApplicationConfig& config, std::unique_ptr<NGraphics::Graphics> graphics);
    void PumpPresentation(int width, int height);
    [[nodiscard]] FrameDiagnostics GetDiagnostics() const;

    ~FrameLoop();

    void Start(MainExecutor::Wakeup wakeup = {});
    [[nodiscard]] MainExecutor& GetMainExecutor() noexcept {
        return m_mainExecutor;
    }

    void Stop() noexcept;
    void CheckError() const;

    bool Step(IFrameLoopCallbacks& callbacks);
    void WaitForProgress() const;

    void EngineUpdateCheckpoint();

    void EngineDrawCheckpoint();

private:
    class EngineStopGuard;

    enum class EFrameCheckpoint {
        READY_FOR_USER_UPDATE,
        WAITING_ENGINE_UPDATE,
        READY_FOR_USER_DRAW,
        WAITING_ENGINE_DRAW,
    };

private:
    MainExecutor m_mainExecutor;
    std::unique_ptr<IEngineFactory> m_engineFactory;
    std::unique_ptr<NEngine::Engine> m_engine;
    PresentationRuntime* m_presentation = nullptr;
    EFrameCheckpoint m_frameCheckpoint = EFrameCheckpoint::READY_FOR_USER_UPDATE;
    std::optional<NEngine::FrameKey> m_reservedFrame;
    std::uint64_t m_progressVersion = 0;
};

} // namespace NApplication::NRuntime
