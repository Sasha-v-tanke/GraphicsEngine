#pragma once

#include <memory>

#include <engine/engine.h>
#include <engine/engine_config.h>

namespace NApplication::NRuntime {

class IEngineFactory {
public:
    virtual ~IEngineFactory() = default;

    [[nodiscard]] virtual std::unique_ptr<NEngine::Engine> Create(NEngine::EngineConfig config) = 0;
};

class DefaultEngineFactory final: public IEngineFactory {
public:
    [[nodiscard]] std::unique_ptr<NEngine::Engine> Create(NEngine::EngineConfig config) override;
};

[[nodiscard]] std::unique_ptr<NEngine::Engine> CreateEngine(IEngineFactory& engineFactory,
                                                            NEngine::EngineConfig config);

} // namespace NApplication::NRuntime
