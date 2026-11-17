#include "view_registry.h"

#include <atomic>
#include <limits>

#include <GraphicsEngine/lib/common/error/exception.h>

namespace NApplication {
namespace {
std::uint64_t AcquireOwner() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "ViewRegistry identity space exhausted");
        }
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
            return value;
        }
    }
}
} // namespace

ViewRegistry::ViewRegistry()
    : m_owner(AcquireOwner())
    , m_thread(std::this_thread::get_id())
    , m_entries(1) {
}

void ViewRegistry::ValidateThread() const {
    if (m_thread != std::this_thread::get_id()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "ViewRegistry requires its owner thread");
    }
}

bool ViewRegistry::Contains(ViewHandle view) const noexcept {
    return view.m_owner == m_owner && view.m_slot < m_entries.size() && m_entries[view.m_slot].Live &&
           m_entries[view.m_slot].Generation == view.m_generation;
}

void ViewRegistry::Validate(ViewHandle view) const {
    ValidateThread();
    if (!Contains(view)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_FOUND, "Stale or foreign view handle");
    }
}

bool ViewRegistry::IsRegistered(ViewHandle view) const {
    ValidateThread();
    return Contains(view);
}

ViewHandle ViewRegistry::GetMainView() const {
    ValidateThread();
    return {m_owner, 0, m_entries[0].Generation};
}

ViewHandle ViewRegistry::CreateView() {
    ValidateThread();
    for (std::size_t i = 1; i < m_entries.size(); ++i) {
        auto& entry = m_entries[i];
        if (!entry.Live && entry.Generation != std::numeric_limits<std::uint64_t>::max()) {
            const auto generation = entry.Generation + 1;
            entry = Entry{};
            entry.Generation = generation;
            return {m_owner, i, entry.Generation};
        }
    }
    m_entries.emplace_back();
    return {m_owner, m_entries.size() - 1, 1};
}

void ViewRegistry::DestroyView(ViewHandle view) {
    Validate(view);
    if (view.m_slot == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Main view is reserved");
    }
    auto& entry = m_entries[view.m_slot];
    entry.Live = false;
    entry.Camera.reset();
}

void ViewRegistry::SetFramebufferSize(ViewHandle view, std::uint32_t width, std::uint32_t height) {
    Validate(view);
    m_entries[view.m_slot].Width = width;
    m_entries[view.m_slot].Height = height;
}

void ViewRegistry::BindCamera(ViewHandle view,
                              const NScene::SceneManager& scenes,
                              NScene::SceneHandle scene,
                              NEcs::EntityReference camera) {
    Validate(view);
    const auto source = scenes.Acquire(scene);
    const auto& world = source->GetWorld();
    if (world.GetEntityReference(camera.Identity) != camera || !world.IsAlive(camera.Identity)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_FOUND, "Camera identity is stale or belongs to another Scene");
    }
    m_entries[view.m_slot].Camera = Binding{scene, camera};
}

void ViewRegistry::UnbindCamera(ViewHandle view) {
    Validate(view);
    m_entries[view.m_slot].Camera.reset();
}

ViewSnapshot
ViewRegistry::Publish(ViewHandle view, const NScene::SceneManager& scenes, const NRenderer::RenderWorld& world) const {
    Validate(view);
    const auto& entry = m_entries[view.m_slot];
    ViewSnapshot result;
    result.m_view = view;
    std::optional<NRenderer::RenderView> selected;
    if (entry.Camera) {
        result.m_scene = entry.Camera->Scene;
        result.m_identity = entry.Camera->Camera;
    }
    if (entry.Width == 0 || entry.Height == 0) {
        result.m_diagnostic = EViewDiagnostic::ZERO_EXTENT;
    } else if (!entry.Camera) {
        result.m_diagnostic = EViewDiagnostic::UNBOUND;
    } else if (!scenes.IsRegistered(entry.Camera->Scene)) {
        result.m_diagnostic = EViewDiagnostic::STALE_SCENE;
    } else if (!scenes.GetActiveHandle()) {
        result.m_diagnostic = EViewDiagnostic::NO_ACTIVE_SCENE;
    } else if (scenes.GetActiveHandle() != entry.Camera->Scene) {
        result.m_diagnostic = EViewDiagnostic::INACTIVE_SCENE;
    } else if (world.GetDelta().WorldId != entry.Camera->Camera.WorldId) {
        result.m_diagnostic = EViewDiagnostic::SOURCE_MISMATCH;
    } else {
        const auto source = scenes.Acquire(entry.Camera->Scene);
        const auto& ecs = source->GetWorld();
        const auto identity = entry.Camera->Camera;
        result.m_diagnostic = EViewDiagnostic::CAMERA_UNAVAILABLE;
        if (ecs.IsAlive(identity.Identity) && ecs.IsEntityEnabled(identity) &&
            ecs.IsComponentEnabled<NRenderer::CameraComponent>(identity) &&
            ecs.IsComponentEnabled<NRenderer::TransformComponent>(identity)) {
            for (const auto& camera: world.GetViews()) {
                if (camera.Id.Index == identity.Identity.Index &&
                    camera.Id.Generation == identity.Identity.Generation) {
                    selected = camera;
                    selected->Projection.AspectRatio =
                            static_cast<float>(entry.Width) / static_cast<float>(entry.Height);
                    result.m_diagnostic = EViewDiagnostic::NONE;
                    break;
                }
            }
        }
    }
    result.m_selection = {world.GetFrame(), entry.Width, entry.Height, selected};
    return result;
}
} // namespace NApplication
