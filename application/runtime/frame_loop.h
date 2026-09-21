#pragma once

#include <memory>

#include <application/application_config.h>
#include <application/runtime/engine_factory.h>
#include <engine/engine.h>

namespace NApplication::NRuntime {

class IFrameLoopCallbacks {
public:
    virtual ~IFrameLoopCallbacks() = default;

    virtual void OnUpdate() = 0;

    virtual void OnDraw() = 0;
};

class FrameLoop final: public NCommon::NonTransferable {
public:
    explicit FrameLoop(const ApplicationConfig& config);

    FrameLoop(const ApplicationConfig& config, std::unique_ptr<IEngineFactory> engineFactory);

    ~FrameLoop();

    void Start();

    void Stop() noexcept;

    void Step(IFrameLoopCallbacks& callbacks);

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
    std::unique_ptr<IEngineFactory> m_engineFactory;
    std::unique_ptr<NEngine::Engine> m_engine;
    EFrameCheckpoint m_frameCheckpoint = EFrameCheckpoint::READY_FOR_USER_UPDATE;
};

} // namespace NApplication::NRuntime
