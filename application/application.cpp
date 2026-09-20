#include "application.h"

#include <memory>
#include <utility>

#include <engine/engine.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>
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

Application::Application(const ApplicationConfig& config)
    : m_window(std::make_unique<ApplicationWindow>(*this, config.Window))
    , m_engine(std::make_unique<NEngine::Engine>(NEngine::EngineConfig{
              .MaxActiveFrames = config.MaxActiveFrames,
              .WorkerCount = config.WorkerCount,
      })) {
}

Application::~Application() {
    if (m_engine != nullptr) {
        m_engine->Stop();
        m_engine.reset();
    }

    m_window.reset();
}

void Application::Run() {
    m_engine->Start();

    while (!m_window->ShouldClose()) {
        RunFrame();
    }

    m_engine->Stop();
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

void Application::EngineUpdateCheckpoint() {
    if (m_frameCheckpoint != EFrameCheckpoint::READY_FOR_UPDATE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application update checkpoint is out of order");
    }

    static_cast<void>(m_engine->Update());
    m_frameCheckpoint = EFrameCheckpoint::READY_FOR_DRAW;
}

void Application::EngineDrawCheckpoint() {
    if (m_frameCheckpoint != EFrameCheckpoint::READY_FOR_DRAW) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application draw checkpoint is out of order");
    }

    static_cast<void>(m_engine->Draw());
    m_frameCheckpoint = EFrameCheckpoint::READY_FOR_UPDATE;
}

NWindow::Window& Application::GetWindow() noexcept {
    return *m_window;
}

const NWindow::Window& Application::GetWindow() const noexcept {
    return *m_window;
}

NEngine::Engine& Application::GetEngine() noexcept {
    return *m_engine;
}

const NEngine::Engine& Application::GetEngine() const noexcept {
    return *m_engine;
}

void Application::RunFrame() {
    m_window->ProcessEvents();

    if (m_window->ShouldClose()) {
        return;
    }

    OnUpdate();
    EngineUpdateCheckpoint();

    OnDraw();
    EngineDrawCheckpoint();
}

} // namespace NApplication
