#pragma once

#include <memory>

#include <engine/engine.h>
#include <engine/engine_config.h>

namespace NApplication::NInternal {

using EngineFactory = std::unique_ptr<NEngine::Engine> (*)(NEngine::EngineConfig config);

[[nodiscard]] std::unique_ptr<NEngine::Engine> CreateEngine(NEngine::EngineConfig config);

void SetEngineFactoryForTests(EngineFactory factory);

} // namespace NApplication::NInternal
