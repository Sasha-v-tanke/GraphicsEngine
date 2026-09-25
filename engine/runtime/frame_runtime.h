#pragma once

#include <memory>

#include <engine/controller/frame_scheduler.h>
#include <engine/engine.h>
#include <engine/engine_config.h>

namespace NEngine::NRuntime {

class FrameContext final {
public:
    explicit FrameContext(NController::FrameHandle frame) noexcept
        : m_frame(frame) {
    }

    [[nodiscard]] NController::FrameHandle GetFrame() const noexcept {
        return m_frame;
    }

private:
    NController::FrameHandle m_frame;
};

class IFrameRuntime {
public:
    virtual ~IFrameRuntime() = default;

    virtual void Update(const FrameContext& frame) = 0;

    virtual void Draw(const FrameContext& frame) = 0;
};

[[nodiscard]] std::unique_ptr<IFrameRuntime> MakeDefaultFrameRuntime();

class EngineFactory final {
public:
    [[nodiscard]] static std::unique_ptr<Engine> Create(EngineConfig config,
                                                        std::unique_ptr<IFrameRuntime> frameRuntime);
};

} // namespace NEngine::NRuntime
