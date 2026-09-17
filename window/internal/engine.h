#pragma once

#include <string_view>

#include <window/window_size.h>

namespace NWindow::NInternal {

class IWindowEventSink;

class IWindowEngine {
public:
    virtual ~IWindowEngine() = default;

    // Attaches the event sink without emitting callbacks synchronously.
    virtual void AttachEventSink(IWindowEventSink& eventSink) = 0;

    virtual void SetTitle(std::string_view title) = 0;

    virtual void SetSize(WindowSize size) = 0;

    [[nodiscard]] virtual WindowSize GetSize() const = 0;

    [[nodiscard]] virtual WindowSize GetFramebufferSize() const = 0;

    [[nodiscard]] virtual bool ShouldClose() const = 0;

    virtual void RequestClose() = 0;

    virtual void ProcessEvents() = 0;
};

} // namespace NWindow::NInternal
