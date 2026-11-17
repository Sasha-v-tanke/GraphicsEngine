#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include <GraphicsEngine/renderer/view_selection.h>
#include <GraphicsEngine/scene/scene_manager.h>

namespace NApplication {

class ViewHandle final {
public:
    ViewHandle() = default;
    [[nodiscard]] bool IsValid() const noexcept {
        return m_owner != 0 && m_generation != 0;
    }
    [[nodiscard]] friend bool operator==(ViewHandle, ViewHandle) noexcept = default;

private:
    ViewHandle(std::uint64_t owner, std::size_t slot, std::uint64_t generation)
        : m_owner(owner)
        , m_slot(slot)
        , m_generation(generation) {
    }
    std::uint64_t m_owner = 0;
    std::size_t m_slot = 0;
    std::uint64_t m_generation = 0;
    friend class ViewRegistry;
};

enum class EViewDiagnostic {
    NONE,
    ZERO_EXTENT,
    UNBOUND,
    NO_ACTIVE_SCENE,
    INACTIVE_SCENE,
    STALE_SCENE,
    CAMERA_UNAVAILABLE,
    SOURCE_MISMATCH,
};

// Value-owned frame publication: no references to registry, Scene, World or extraction storage.
// A missing camera means clear-only; graph builders must not fall back to the first extracted camera.
class ViewSnapshot final {
public:
    [[nodiscard]] ViewHandle GetView() const noexcept {
        return m_view;
    }
    [[nodiscard]] NRenderer::RenderFrameIdentity GetFrame() const noexcept {
        return m_selection.GetFrame();
    }
    [[nodiscard]] std::uint32_t GetWidth() const noexcept {
        return m_selection.GetWidth();
    }
    [[nodiscard]] std::uint32_t GetHeight() const noexcept {
        return m_selection.GetHeight();
    }
    [[nodiscard]] EViewDiagnostic GetDiagnostic() const noexcept {
        return m_diagnostic;
    }
    [[nodiscard]] const std::optional<NRenderer::RenderView>& GetCamera() const noexcept {
        return m_selection.GetCamera();
    }
    [[nodiscard]] std::optional<NScene::SceneHandle> GetScene() const noexcept {
        return m_scene;
    }
    [[nodiscard]] std::optional<NEcs::EntityReference> GetCameraIdentity() const noexcept {
        return m_identity;
    }

    [[nodiscard]] const NRenderer::ViewSelection& GetSelection() const noexcept {
        return m_selection;
    }

private:
    ViewHandle m_view;
    NRenderer::ViewSelection m_selection;
    EViewDiagnostic m_diagnostic = EViewDiagnostic::UNBOUND;
    std::optional<NScene::SceneHandle> m_scene;
    std::optional<NEcs::EntityReference> m_identity;
    friend class ViewRegistry;
};

// Application-owned, owner-thread registry. Publish requires a quiescent owner phase, after
// extraction of the active Scene; no concurrent World mutation through the end of this call.
// Main framebuffer extent is supplied by Application after window event processing.
class ViewRegistry final: private NCommon::NonTransferable {
public:
    ViewRegistry();
    [[nodiscard]] ViewHandle GetMainView() const;
    [[nodiscard]] ViewHandle CreateView(); // Logical views only; does not create a window/output.
    void DestroyView(ViewHandle view);     // Reserved main view cannot be destroyed.
    [[nodiscard]] bool IsRegistered(ViewHandle view) const;
    void SetFramebufferSize(ViewHandle view, std::uint32_t width, std::uint32_t height);
    void BindCamera(ViewHandle view,
                    const NScene::SceneManager& scenes,
                    NScene::SceneHandle scene,
                    NEcs::EntityReference camera);
    void UnbindCamera(ViewHandle view);
    [[nodiscard]] ViewSnapshot
    Publish(ViewHandle view, const NScene::SceneManager& scenes, const NRenderer::RenderWorld& world) const;

private:
    struct Binding {
        NScene::SceneHandle Scene;
        NEcs::EntityReference Camera;
    };
    struct Entry {
        std::uint64_t Generation = 1;
        bool Live = true;
        std::uint32_t Width = 0;
        std::uint32_t Height = 0;
        std::optional<Binding> Camera;
    };
    void ValidateThread() const;
    [[nodiscard]] bool Contains(ViewHandle view) const noexcept;
    void Validate(ViewHandle view) const;
    std::uint64_t m_owner;
    std::thread::id m_thread;
    std::vector<Entry> m_entries;
};

} // namespace NApplication
