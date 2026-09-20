#include "engine_factory.h"

#include <memory>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NApplication::NInternal {

namespace {

thread_local EngineFactory g_testFactory = nullptr;

[[nodiscard]] std::unique_ptr<NEngine::Engine> ValidateEngine(std::unique_ptr<NEngine::Engine> engine) {
    if (engine == nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application engine factory returned null");
    }

    return engine;
}

} // namespace

std::unique_ptr<NEngine::Engine> CreateEngine(NEngine::EngineConfig config) {
    if (g_testFactory != nullptr) {
        return ValidateEngine(g_testFactory(config));
    }

    return std::make_unique<NEngine::Engine>(config);
}

void SetEngineFactoryForTests(EngineFactory factory) {
    g_testFactory = factory;
}

} // namespace NApplication::NInternal
