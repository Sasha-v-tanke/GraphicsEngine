#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string_view>

#include <window/window_size.h>

namespace NWindow::NEngine {

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

    // Owner-thread event wait. Platforms override this to also wake for native events.
    virtual void WaitEvents(std::chrono::milliseconds timeout) {
        {
            std::unique_lock lock{m_eventMutex};
            m_eventChanged.wait_for(lock, timeout, [this] { return m_eventPending; });
            m_eventPending = false;
        }
        ProcessEvents();
    }

    // Only thread-safe platform signalling is permitted here; never dispatch user callbacks.
    virtual void WakeEventLoop() noexcept {
        {
            std::lock_guard lock{m_eventMutex};
            m_eventPending = true;
        }
        m_eventChanged.notify_one();
    }

private:
    std::mutex m_eventMutex;
    std::condition_variable m_eventChanged;
    bool m_eventPending = false;
};

} // namespace NWindow::NEngine
