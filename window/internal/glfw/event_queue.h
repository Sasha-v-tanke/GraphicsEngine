#pragma once

#include <vector>

#include <window/internal/event_sink.h>
#include <window/window_size.h>

namespace NWindow::NInternal::NGlfw {

enum class EWindowEventType {
    RESIZE,
    FRAMEBUFFER_RESIZE,
    CLOSE,
};

struct WindowEvent {
    EWindowEventType Type;
    WindowSize Size{};
};

class WindowEventQueue {
public:
    void Enqueue(WindowEvent event) {
        m_events.push_back(event);
    }

    [[nodiscard]] bool IsEmpty() const {
        return m_events.empty();
    }

    void Dispatch(IWindowEventSink& eventSink) {
        std::vector<WindowEvent> events;
        events.swap(m_events);

        for (const WindowEvent& event: events) {
            switch (event.Type) {
            case EWindowEventType::RESIZE:
                eventSink.HandleResize(event.Size);
                break;
            case EWindowEventType::FRAMEBUFFER_RESIZE:
                eventSink.HandleFramebufferResize(event.Size);
                break;
            case EWindowEventType::CLOSE:
                eventSink.HandleClose();
                break;
            }
        }
    }

private:
    std::vector<WindowEvent> m_events;
};

} // namespace NWindow::NInternal::NGlfw
