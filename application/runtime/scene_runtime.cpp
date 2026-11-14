#include "scene_runtime.h"

#include <utility>

#include <engine/runtime/world_update.h>
#include <lib/common/error/exception.h>
#include <renderer/extraction.h>

namespace NApplication::NRuntime {

struct SceneRuntime::Binding {
    ScenePublication Publication;
    std::shared_ptr<NScene::Scene> Source;
    const NRenderer::RenderWorld* Snapshot = nullptr;
    NCommon::TaskSystem* Tasks = nullptr;
};

SceneRuntime::SceneRuntime(NResources::ResourceManager& resources, Consumer consumer, std::function<void()> drain)
    : m_resources(resources)
    , m_consumer(std::move(consumer))
    , m_drain(std::move(drain)) {
    if (!m_consumer) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Scene consumer is empty");
    }
}

std::optional<NEngine::FrameKey>
SceneRuntime::TryBeginFrame(NEngine::Engine& engine,
                            NScene::SceneManager& scenes,
                            const std::function<void(NScene::SceneManager&)>& publish) {
    if (std::this_thread::get_id() != m_owner || (m_engine != nullptr && m_engine != &engine)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Scene publication requires its owner Engine/thread");
    }
    const auto frame = engine.TryBeginFrame();
    if (!frame) {
        return {};
    }
    try {
        if (publish) {
            publish(scenes);
        }
        auto binding = std::make_shared<Binding>();
        binding->Publication.Frame = *frame;
        binding->Publication.Scene = scenes.GetActiveHandle();
        binding->Source = scenes.AcquireActive();
        if (binding->Source) {
            binding->Publication.Name = binding->Source->GetName();
        }
        // Replacement only follows Engine admission into this physical slot: all its old GPU users retired.
        // Destroy the previous binding outside the metadata lock, including exceptional paths.
        std::shared_ptr<Binding> retired;
        {
            std::lock_guard lock{m_mutex};
            if (m_slots.size() <= frame->GetSlotIndex()) {
                m_slots.resize(frame->GetSlotIndex() + 1);
            }
            retired = std::exchange(m_slots[frame->GetSlotIndex()], std::move(binding));
            m_engine = &engine;
        }
    } catch (...) {
        (void)engine.AbortFrame(*frame);
        throw;
    }
    return frame;
}

std::shared_ptr<SceneRuntime::Binding> SceneRuntime::GetBinding(const NEngine::NRuntime::FrameContext& frame) {
    std::lock_guard lock{m_mutex};
    const auto handle = frame.GetFrame();
    if (handle.GetSlotIndex() < m_slots.size()) {
        const auto& binding = m_slots[handle.GetSlotIndex()];
        if (binding && binding->Publication.Frame.GetFrameSequence() == handle.GetFrameIndex() &&
            binding->Publication.Frame.GetGeneration() == handle.GetGeneration()) {
            return binding;
        }
    }
    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Frame has no matching scene publication");
}

void SceneRuntime::Update(const NEngine::NRuntime::FrameContext&) {
    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Scene Update requires scheduled completion");
}

void SceneRuntime::ScheduleUpdate(const NEngine::NRuntime::FrameContext& frame,
                                  NCommon::TaskSystem& tasks,
                                  std::shared_ptr<NEngine::NRuntime::UpdateCompletion> completion) {
    const auto binding = GetBinding(frame);
    binding->Tasks = &tasks;
    if (!binding->Source) {
        completion->Finish();
        return;
    }
    auto world = std::shared_ptr<NEcs::World>{binding->Source, &binding->Source->GetWorld()};
    NEngine::NRuntime::ScheduleWorldUpdate(std::move(world), tasks, completion);
}

NCommon::TaskHandle SceneRuntime::ScheduleExtract(const NEngine::NRuntime::FrameContext& frame,
                                                  NCommon::TaskContext& task) {
    const auto binding = GetBinding(frame);
    const auto source = binding->Source;
    auto& world = source ? source->GetWorld() : m_empty;
    return m_extraction.Schedule(world,
                                 m_resources,
                                 frame.GetStorage(),
                                 *binding->Tasks,
                                 task,
                                 [binding, source](const NRenderer::RenderWorld& snapshot) {
                                     binding->Snapshot = &snapshot;
                                     // The closure still pins Source through extraction capture retirement. Draw only
                                     // reads Snapshot.
                                     binding->Source.reset();
                                 });
}

void SceneRuntime::Draw(const NEngine::NRuntime::FrameContext& frame) {
    const auto binding = GetBinding(frame);
    if (binding->Snapshot == nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Scene snapshot was not extracted");
    }
    const auto& plan = NRenderer::Renderer::Prepare(*binding->Snapshot, frame.GetMemoryResource());
    m_consumer(frame, binding->Publication, *binding->Snapshot, plan);
}

void SceneRuntime::Drain() noexcept {
    m_extraction.Reset();
    if (m_drain) {
        m_drain();
    }
    std::vector<std::shared_ptr<Binding>> retired;
    {
        std::lock_guard lock{m_mutex};
        retired.swap(m_slots);
    }
}

} // namespace NApplication::NRuntime
