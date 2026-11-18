#pragma once

#include <memory>

#include <engine/engine.h>
#include <engine/engine_config.h>

namespace NApplication::NRuntime {

class IEngineFactory {
public:
    virtual ~IEngineFactory() = default;

    [[nodiscard]] virtual std::unique_ptr<NEngine::Engine> Create(NEngine::EngineConfig config) = 0;

    // Owner-thread admission hook. Game composition publishes Scene only after successful reservation.
    [[nodiscard]] virtual std::optional<NEngine::FrameKey> TryBeginFrame(NEngine::Engine& engine) {
        return engine.TryBeginFrame();
    }

    // Roll back runtime publication only when Engine confirms that this reservation has no CPU consumers.
    [[nodiscard]] virtual bool AbortFrame(NEngine::Engine& engine, NEngine::FrameKey frame) {
        return engine.AbortFrame(frame);
    }
};

class DefaultEngineFactory final: public IEngineFactory {
public:
    [[nodiscard]] std::unique_ptr<NEngine::Engine> Create(NEngine::EngineConfig config) override;
};

[[nodiscard]] std::unique_ptr<NEngine::Engine> CreateEngine(IEngineFactory& engineFactory,
                                                            NEngine::EngineConfig config);

} // namespace NApplication::NRuntime
