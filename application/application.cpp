#include "application.h"

#include <memory>

#include <application/runtime/frame_loop.h>
#include <window/window.h>

namespace NApplication {

class Application::ApplicationWindow final: public NWindow::Window {
public:
    ApplicationWindow(Application& application, const NWindow::WindowConfig& config)
        : Window(config)
        , m_application(application) {
    }

private:
    void OnResize(NWindow::WindowSize size) override {
        m_application.OnResize(size);
    }

    void OnFramebufferResize(NWindow::WindowSize size) override {
        m_application.OnFramebufferResize(size);
    }

    void OnClose() override {
        m_application.OnClose();
    }

private:
    Application& m_application;
};

class Application::FrameLoopCallbacks final: public NRuntime::IFrameLoopCallbacks {
public:
    explicit FrameLoopCallbacks(Application& application) noexcept
        : m_application(application) {
    }

    void OnUpdate() override {
        m_application.OnUpdate();
    }

    void OnDraw() override {
        m_application.OnDraw();
    }

private:
    Application& m_application;
};

Application::Application(const ApplicationConfig& config)
    : m_window(std::make_unique<ApplicationWindow>(*this, config.Window))
    , m_frameLoop(std::make_unique<NRuntime::FrameLoop>(config)) {
}

Application::~Application() {
    if (m_frameLoop != nullptr) {
        m_frameLoop->Stop();
        m_frameLoop.reset();
    }

    m_window.reset();
}

void Application::Run() {
    m_frameLoop->Start();

    try {
        while (!m_window->ShouldClose()) {
            RunFrame();
        }
    } catch (...) {
        m_frameLoop->Stop();
        throw;
    }

    m_frameLoop->Stop();
}

void Application::RequestShutdown() {
    m_window->RequestClose();
}

bool Application::IsShutdownRequested() const {
    return m_window->ShouldClose();
}

void Application::OnUpdate() {
}

void Application::OnDraw() {
}

void Application::OnResize(NWindow::WindowSize) {
}

void Application::OnFramebufferResize(NWindow::WindowSize) {
}

void Application::OnClose() {
}

NWindow::Window& Application::GetWindow() noexcept {
    return *m_window;
}

const NWindow::Window& Application::GetWindow() const noexcept {
    return *m_window;
}

void Application::RunFrame() {
    m_window->ProcessEvents();

    if (m_window->ShouldClose()) {
        return;
    }

    FrameLoopCallbacks callbacks{*this};
    m_frameLoop->Step(callbacks);
}

} // namespace NApplication
