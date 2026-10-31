#include "application.h"

#include <chrono>
#include <memory>
#include <utility>

#include <application/runtime/frame_loop.h>
#include <graphics/presentation.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>
#include <window/engine/engine.h>
#include <window/internal/window_engine_access.h>
#include <window/window.h>
#include <window/window_runtime.h>

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
        m_application.m_frameLoop->GetMainExecutor().Stop();
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

    [[nodiscard]] bool IsShutdownRequested() const override {
        return m_application.IsShutdownRequested();
    }

private:
    Application& m_application;
};

Application::Application(const ApplicationConfig& config)
    : m_hasPresentation(config.Presentation.has_value())
    , m_windowRuntime(std::make_unique<NWindow::WindowRuntime>())
    , m_window(std::make_unique<ApplicationWindow>(*this, config.Window))
    , m_frameLoop([this, &config] {
        (void)this;
        if (!config.Presentation) {
            return std::make_unique<NRuntime::FrameLoop>(config);
        }
#if defined(GRAPHICS_ENGINE_HAS_PRESENTATION)
        auto presentation = *config.Presentation;
        presentation.MaxActiveFrames = config.MaxActiveFrames;
        return std::make_unique<NRuntime::FrameLoop>(config,
                                                     NGraphics::CreatePresentationGraphics(*m_window, presentation));
#else
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Presentation requires Vulkan and GLFW");
#endif
    }()) {
}

void Application::SubmitUserJob(std::function<void()> function) {
    m_frameLoop->SubmitUserJob(std::move(function));
}

void Application::RunOnMainThreadImpl(void* context, void (*invoke)(void*)) {
    m_frameLoop->GetMainExecutor().Invoke([context, invoke] { invoke(context); });
}

Application::~Application() {
    if (m_frameLoop != nullptr) {
        m_frameLoop->Stop();
        m_frameLoop.reset();
    }

    m_window.reset();
    m_windowRuntime.reset();
}

void Application::Run() {
    m_frameLoop->Start({m_window.get(), [](void* window) noexcept {
                            NWindow::NInternal::GetWindowEngine(*static_cast<NWindow::Window*>(window)).WakeEventLoop();
                        }});

    try {
        while (!m_window->ShouldClose()) {
            RunFrame();
        }
    } catch (...) {
        m_frameLoop->Stop();
        throw;
    }

    m_frameLoop->Stop();
    m_frameLoop->CheckError();
}

void Application::RequestShutdown() {
    m_window->RequestClose();
    m_frameLoop->GetMainExecutor().Stop();
}

FrameDiagnostics Application::GetFrameDiagnostics() const {
    return m_frameLoop->GetDiagnostics();
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

    const auto size = m_window->GetFramebufferSize();
    m_frameLoop->PumpPresentation(size.Width, size.Height);
    FrameLoopCallbacks callbacks{*this};
    const bool progressed = m_frameLoop->Step(callbacks);
    // Bounded platform/completion maintenance; admission retries never repeat user callbacks.
    if (!m_window->ShouldClose() && (!progressed || m_hasPresentation)) {
        NWindow::NInternal::GetWindowEngine(*m_window).WaitEvents(std::chrono::milliseconds{progressed ? 1 : 8});
    }
}

} // namespace NApplication
