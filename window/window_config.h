#pragma once

#include <string>

#include <GraphicsEngine/window/window_size.h>
#include <GraphicsEngine/window/window_type.h>

namespace NWindow {

struct WindowConfig {
    explicit WindowConfig(EWindowType type)
        : Type(type) {
    }

    // Empty titles are passed through to the backend.
    std::string Title = "GraphicsEngine";

    // Backends may clamp unsupported non-positive sizes before native window creation.
    WindowSize Size{
            .Width = 1280,
            .Height = 720,
    };

    // Unsupported types throw during Window construction.
    EWindowType Type;
};

} // namespace NWindow
