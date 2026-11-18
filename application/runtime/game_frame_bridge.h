#pragma once

#include <functional>

#include <application/runtime/engine_factory.h>
#include <application/runtime/scene_runtime.h>

namespace NApplication::NRuntime {

// Internal FrameLoop composition. Borrowed services/consumer targets outlive FrameLoop::Stop.
// Engine exclusively owns SceneRuntime and its TaskSystem; this factory only routes owner-thread admission.
class GameFrameBridge final: public IEngineFactory {
public:
    using Publication = std::function<void(NScene::SceneManager&)>;

    GameFrameBridge(NScene::SceneManager& scenes,
                    NResources::ResourceManager& resources,
                    SceneRuntime::Consumer consumer,
                    Publication publish = {},
                    std::function<void()> drain = {});

    [[nodiscard]] std::unique_ptr<NEngine::Engine> Create(NEngine::EngineConfig config) override;
    [[nodiscard]] std::optional<NEngine::FrameKey> TryBeginFrame(NEngine::Engine& engine) override;
    [[nodiscard]] bool AbortFrame(NEngine::Engine& engine, NEngine::FrameKey frame) override;

private:
    NScene::SceneManager& m_scenes;
    NResources::ResourceManager& m_resources;
    SceneRuntime::Consumer m_consumer;
    Publication m_publish;
    std::function<void()> m_drain;
    SceneRuntime* m_runtime = nullptr;
    NEngine::Engine* m_engine = nullptr;
};

} // namespace NApplication::NRuntime
