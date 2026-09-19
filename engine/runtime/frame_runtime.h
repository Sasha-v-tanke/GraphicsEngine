#pragma once

#include <memory>

#include <engine/controller/frame_scheduler.h>
#include <engine/engine.h>
#include <engine/engine_config.h>

namespace NEngine::NRuntime {

class IFrameRuntime {
public:
    virtual ~IFrameRuntime() = default;

    virtual void Update(NController::FrameScheduler& frameScheduler, NController::FrameHandle frame) = 0;

    virtual void Draw(NController::FrameScheduler& frameScheduler, NController::FrameHandle frame) = 0;
};

[[nodiscard]] std::unique_ptr<IFrameRuntime> MakeDefaultFrameRuntime();

class EngineFactory final {
public:
    [[nodiscard]] static std::unique_ptr<Engine> Create(EngineConfig config,
                                                        std::unique_ptr<IFrameRuntime> frameRuntime);
};

} // namespace NEngine::NRuntime
