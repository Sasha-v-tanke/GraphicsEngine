#include "game_frame_bridge.h"

#include <utility>

#include <lib/common/error/exception.h>

namespace NApplication::NRuntime {

GameFrameBridge::GameFrameBridge(NScene::SceneManager& scenes,
                                 NResources::ResourceManager& resources,
                                 SceneRuntime::Consumer consumer,
                                 Publication publish,
                                 std::function<void()> drain)
    : m_scenes(scenes)
    , m_resources(resources)
    , m_consumer(std::move(consumer))
    , m_publish(std::move(publish))
    , m_drain(std::move(drain)) {
}

std::unique_ptr<NEngine::Engine> GameFrameBridge::Create(NEngine::EngineConfig config) {
    if (m_engine != nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Game bridge already owns an Engine binding");
    }
    auto runtime = std::make_unique<SceneRuntime>(m_resources, m_consumer, m_drain);
    auto* host = runtime.get();
    auto engine = NEngine::NRuntime::EngineFactory::Create(config, std::move(runtime));
    m_runtime = host;
    m_engine = engine.get();
    return engine;
}

std::optional<NEngine::FrameKey> GameFrameBridge::TryBeginFrame(NEngine::Engine& engine) {
    if (m_runtime == nullptr || m_engine != &engine) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Game bridge requires its bound Engine");
    }
    return m_runtime->TryBeginFrame(engine, m_scenes, m_publish);
}

} // namespace NApplication::NRuntime
