#include "window.h"

#include <memory>
#include <string_view>

#include <window/internal/engine.h>
#include <window/internal/event_sink.h>
#include <window/internal/factory.h>

namespace NWindow {

class Window::Impl final: public NInternal::IWindowEventSink {
public:
    Impl(Window& window, const WindowConfig& config)
        : m_window(window)
        , m_engine(NInternal::CreateWindowEngine(config, *this)) {
    }

    void SetTitle(std::string_view title) {
        m_engine->SetTitle(title);
    }

    void SetSize(WindowSize size) {
        m_engine->SetSize(size);
    }

    [[nodiscard]] WindowSize GetSize() const {
        return m_engine->GetSize();
    }

    [[nodiscard]] WindowSize GetFramebufferSize() const {
        return m_engine->GetFramebufferSize();
    }

    [[nodiscard]] bool ShouldClose() const {
        return m_engine->ShouldClose();
    }

    void RequestClose() {
        if (m_engine->ShouldClose()) {
            NotifyClose();
            return;
        }

        m_engine->RequestClose();

        if (m_engine->ShouldClose()) {
            NotifyClose();
        }
    }

    void ProcessEvents() {
        m_engine->ProcessEvents();
    }

private:
    void HandleResize(WindowSize size) override {
        m_window.OnResize(size);
    }

    void HandleFramebufferResize(WindowSize size) override {
        m_window.OnFramebufferResize(size);
    }

    void HandleClose() override {
        NotifyClose();
    }

    void NotifyClose() {
        if (m_closeNotified) {
            return;
        }

        m_closeNotified = true;

        m_window.OnClose();
    }

private:
    Window& m_window;

    std::unique_ptr<NInternal::IWindowEngine> m_engine;

    bool m_closeNotified = false;
};

Window::Window(const WindowConfig& config)
    : m_impl(std::make_unique<Impl>(*this, config)) {
}

Window::~Window() = default;

void Window::SetTitle(std::string_view title) {
    m_impl->SetTitle(title);
}

void Window::SetSize(WindowSize size) {
    m_impl->SetSize(size);
}

WindowSize Window::GetSize() const {
    return m_impl->GetSize();
}

WindowSize Window::GetFramebufferSize() const {
    return m_impl->GetFramebufferSize();
}

bool Window::ShouldClose() const {
    return m_impl->ShouldClose();
}

void Window::RequestClose() {
    m_impl->RequestClose();
}

void Window::ProcessEvents() {
    m_impl->ProcessEvents();
}

void Window::OnResize(WindowSize) {
}

void Window::OnFramebufferResize(WindowSize) {
}

void Window::OnClose() {
}

} // namespace NWindow
