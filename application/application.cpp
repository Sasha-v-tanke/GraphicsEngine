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
        m_application.m_shutdownRequested.store(true);
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

    void CaptureInput() override {
        m_application.m_input = {
                .WindowSize = m_application.m_window->GetSize(),
                .FramebufferSize = m_application.m_window->GetFramebufferSize(),
                .Sequence = m_application.m_input.Sequence + 1,
        };
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
    , m_callbacks(std::make_unique<FrameLoopCallbacks>(*this))
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

void Application::SubmitUserJob(const std::shared_ptr<NCommon::NJobInternal::JobControl>& job) {
    m_frameLoop->SubmitUserJob(job);
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
    m_shutdownRequested.store(m_window->ShouldClose());
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
    const auto close = [this] {
        m_window->RequestClose();
        m_shutdownRequested.store(true);
        m_frameLoop->GetMainExecutor().Stop();
    };
    if (std::this_thread::get_id() == m_applicationThread) {
        close();
    } else {
        RunOnMainThread(close);
    }
}

FrameDiagnostics Application::GetFrameDiagnostics() const {
    return m_frameLoop->GetDiagnostics();
}

bool Application::IsShutdownRequested() const {
    return m_shutdownRequested.load();
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
        m_shutdownRequested.store(true);
        return;
    }

    const auto size = m_window->GetFramebufferSize();
    m_frameLoop->PumpPresentation(size.Width, size.Height);
    const bool progressed = m_frameLoop->Step(*m_callbacks);
    // Bounded platform/completion maintenance; admission retries never repeat user callbacks.
    if (!m_window->ShouldClose() && (!progressed || m_hasPresentation)) {
        NWindow::NInternal::GetWindowEngine(*m_window).WaitEvents(std::chrono::milliseconds{progressed ? 1 : 8});
    }
}

} // namespace NApplication
