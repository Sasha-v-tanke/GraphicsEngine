#pragma once

#include <optional>

#include <GraphicsEngine/renderer/render_world.h>

namespace NRenderer {

// Backend-independent, value-owned input for a single output's graph builder.
// No camera means clear-only. Never select a fallback from RenderWorld::GetViews().
// The graph builder must require GetFrame() == RenderWorld::GetFrame().
class ViewSelection final {
public:
    ViewSelection() = default;
    ViewSelection(RenderFrameIdentity frame,
                  std::uint32_t width,
                  std::uint32_t height,
                  std::optional<RenderView> camera)
        : m_frame(frame)
        , m_width(width)
        , m_height(height)
        , m_camera(camera) {
    }
    [[nodiscard]] RenderFrameIdentity GetFrame() const noexcept {
        return m_frame;
    }
    [[nodiscard]] std::uint32_t GetWidth() const noexcept {
        return m_width;
    }
    [[nodiscard]] std::uint32_t GetHeight() const noexcept {
        return m_height;
    }
    [[nodiscard]] const std::optional<RenderView>& GetCamera() const noexcept {
        return m_camera;
    }

private:
    RenderFrameIdentity m_frame;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::optional<RenderView> m_camera;
};

} // namespace NRenderer
