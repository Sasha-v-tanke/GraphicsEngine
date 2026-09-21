#include "engine_factory.h"

#include <memory>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NApplication::NRuntime {

namespace {

[[nodiscard]] std::unique_ptr<NEngine::Engine> ValidateEngine(std::unique_ptr<NEngine::Engine> engine) {
    if (engine == nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application engine factory returned null");
    }

    return engine;
}

} // namespace

std::unique_ptr<NEngine::Engine> DefaultEngineFactory::Create(NEngine::EngineConfig config) {
    return std::make_unique<NEngine::Engine>(config);
}

std::unique_ptr<NEngine::Engine> CreateEngine(IEngineFactory& engineFactory, NEngine::EngineConfig config) {
    return ValidateEngine(engineFactory.Create(config));
}

} // namespace NApplication::NRuntime
