#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <engine/runtime/frame_runtime.h>
#include <renderer/extraction.h>
#include <renderer/renderer.h>
#include <scene/scene_manager.h>

namespace NApplication::NRuntime {

struct ScenePublication {
    NEngine::FrameKey Frame;
    std::optional<NScene::SceneHandle> Scene;
    std::string Name;
};

// Internal Engine adapter. Resources and consumer outlive Engine::Stop. The consumer runs on workers,
// reads only immutable frame data and registers GPU completion before publishing a presentation intent.
class SceneRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    using Consumer = std::function<void(const NEngine::NRuntime::FrameContext&,
                                        const ScenePublication&,
                                        const NRenderer::RenderWorld&,
                                        const NRenderer::RenderPlan&)>;

    SceneRuntime(NResources::ResourceManager& resources, Consumer consumer, std::function<void()> drain = {});

    // Owner thread only. Publish executes after successful reservation, before any worker sees this frame.
    // Backpressure does not invoke publish. On success the caller completes user Update and requests Draw.
    [[nodiscard]] std::optional<NEngine::FrameKey>
    TryBeginFrame(NEngine::Engine& engine,
                  NScene::SceneManager& scenes,
                  const std::function<void(NScene::SceneManager&)>& publish = {});

    void Update(const NEngine::NRuntime::FrameContext&) override;
    void ScheduleUpdate(const NEngine::NRuntime::FrameContext& frame,
                        NCommon::TaskSystem& tasks,
                        std::shared_ptr<NEngine::NRuntime::UpdateCompletion> completion) override;
    [[nodiscard]] NCommon::TaskHandle ScheduleExtract(const NEngine::NRuntime::FrameContext& frame,
                                                      NCommon::TaskContext& task) override;
    void Draw(const NEngine::NRuntime::FrameContext& frame) override;
    // After Engine joins CPU work, drain retires submitted GPU work on the owner thread; it must not throw.
    void Drain() noexcept override;

private:
    struct Binding;
    [[nodiscard]] std::shared_ptr<Binding> GetBinding(const NEngine::NRuntime::FrameContext& frame);

    NResources::ResourceManager& m_resources;
    Consumer m_consumer;
    std::function<void()> m_drain;
    const std::thread::id m_owner = std::this_thread::get_id();
    NEngine::Engine* m_engine = nullptr;
    std::mutex m_mutex;
    std::vector<std::shared_ptr<Binding>> m_slots;
    NEcs::World m_empty;
    NRenderer::NInternal::IncrementalExtraction m_extraction;
};

} // namespace NApplication::NRuntime
