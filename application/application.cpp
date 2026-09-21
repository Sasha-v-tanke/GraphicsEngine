#include "application.h"

#include <memory>
#include <utility>

#include <application/runtime/engine_factory.h>
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

class Application::EngineStopGuard final {
public:
    explicit EngineStopGuard(NEngine::Engine& engine) noexcept
        : m_engine(engine) {
    }

    ~EngineStopGuard() {
        m_engine.Stop();
    }

    EngineStopGuard(const EngineStopGuard&) = delete;
    EngineStopGuard& operator=(const EngineStopGuard&) = delete;

private:
    NEngine::Engine& m_engine;
};

Application::Application(const ApplicationConfig& config)
    : Application(config, std::make_unique<NRuntime::DefaultEngineFactory>()) {
}

Application::Application(const ApplicationConfig& config, std::unique_ptr<NRuntime::IEngineFactory> engineFactory)
    : m_window(std::make_unique<ApplicationWindow>(*this, config.Window))
    , m_engineFactory(std::move(engineFactory))
    , m_engine([this, &config] {
        if (m_engineFactory == nullptr) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Application engine factory is null");
        }

        return NRuntime::CreateEngine(*m_engineFactory,
                                      NEngine::EngineConfig{
                                              .MaxActiveFrames = config.MaxActiveFrames,
                                              .WorkerCount = config.WorkerCount,
                                      });
    }()) {
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
    m_frameCheckpoint = EFrameCheckpoint::READY_FOR_USER_UPDATE;
    EngineStopGuard stopGuard{*m_engine};

    while (!m_window->ShouldClose()) {
        RunFrame();
    }
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
    if (m_frameCheckpoint != EFrameCheckpoint::WAITING_ENGINE_UPDATE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application update checkpoint is out of order");
    }

    if (m_engine->Update()) {
        m_frameCheckpoint = EFrameCheckpoint::READY_FOR_USER_DRAW;
    }
}

void Application::EngineDrawCheckpoint() {
    if (m_frameCheckpoint != EFrameCheckpoint::WAITING_ENGINE_DRAW) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application draw checkpoint is out of order");
    }

    if (m_engine->Draw()) {
        m_frameCheckpoint = EFrameCheckpoint::READY_FOR_USER_UPDATE;
    }
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

    if (m_frameCheckpoint == EFrameCheckpoint::READY_FOR_USER_UPDATE) {
        OnUpdate();
        m_frameCheckpoint = EFrameCheckpoint::WAITING_ENGINE_UPDATE;
    }

    if (m_frameCheckpoint == EFrameCheckpoint::WAITING_ENGINE_UPDATE) {
        EngineUpdateCheckpoint();

        if (m_frameCheckpoint == EFrameCheckpoint::WAITING_ENGINE_UPDATE) {
            return;
        }
    }

    if (m_frameCheckpoint == EFrameCheckpoint::READY_FOR_USER_DRAW) {
        OnDraw();
        m_frameCheckpoint = EFrameCheckpoint::WAITING_ENGINE_DRAW;
    }

    if (m_frameCheckpoint == EFrameCheckpoint::WAITING_ENGINE_DRAW) {
        EngineDrawCheckpoint();
    }
}

} // namespace NApplication
