#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

namespace NWindow {
class Window;
}

namespace NGraphics {
class Graphics;

struct PresentationConfig {
    std::size_t MaxActiveFrames = 2;
    bool RequireValidation = false;
    // Delivered synchronously by the validation layer; the handler must not throw.
    std::function<void(std::string_view)> ValidationErrorHandler = {};
};

// Narrow Window/Graphics integration. Window outlives Graphics; calls use the application thread.
// Available when Vulkan and GLFW are enabled. Native handles remain inside the backend.
[[nodiscard]] std::unique_ptr<Graphics> CreatePresentationGraphics(NWindow::Window& window,
                                                                   const PresentationConfig& config);
} // namespace NGraphics
