#pragma once

#include <window/window_size.h>

namespace NWindow::NEngine {

class IWindowEventSink {
public:
    virtual ~IWindowEventSink() = default;

    virtual void HandleResize(WindowSize size) = 0;

    virtual void HandleFramebufferResize(WindowSize size) = 0;

    virtual void HandleClose() = 0;
};

} // namespace NWindow::NEngine
