#pragma once

#include <string>

#include <GraphicsEngine/window/window_size.h>
#include <GraphicsEngine/window/window_type.h>

namespace NWindow {

struct WindowConfig {
    explicit WindowConfig(EWindowType type)
        : Type(type) {
    }

    std::string Title = "GraphicsEngine";

    WindowSize Size{
            .Width = 1280,
            .Height = 720,
    };

    EWindowType Type;
};

} // namespace NWindow
