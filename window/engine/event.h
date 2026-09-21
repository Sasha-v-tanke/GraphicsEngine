#pragma once

#include <window/window_size.h>

namespace NWindow::NEngine {

enum class EWindowEventType {
    RESIZE,
    FRAMEBUFFER_RESIZE,
    CLOSE,
};

struct WindowEvent {
    EWindowEventType Type;
    WindowSize Size{};
};

} // namespace NWindow::NEngine
