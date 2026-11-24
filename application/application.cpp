#include "application.h"

#include <chrono>
#include <memory>
#include <utility>

#include <application/runtime/frame_loop.h>
#include <application/runtime/game_output.h>
#include <graphics/presentation.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>
#include <window/engine/engine.h>
#include <window/internal/window_engine_access.h>
#include <window/window.h>
#include <window/window_runtime.h>

namespace NApplication {
namespace {
bool UsesGame(const ApplicationConfig& config) {
    if (!config.Game) {
        return false;
    }
    switch (config.Game->RenderingMode) {
    case ERenderingMode::DISABLED:
        return false;
    case ERenderingMode::STANDARD_3D:
        return true;
    }
    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid Game rendering mode");
}

bool ValidatePresentation(const ApplicationConfig& config) {
    if (config.MaxActiveFrames == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "MaxActiveFrames must be positive");
    }
    return UsesGame(config) || config.Presentation.has_value();
}

std::unique_ptr<ServiceManager> CreateServices() {
    auto services = std::make_unique<ServiceManager>();
    services->Register<NResources::ResourceManager>();
    services->Register<NScene::SceneManager, NResources::ResourceManager>();
    services->Register<ViewRegistry, NScene::SceneManager>();
    return services;
}
} // namespace

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
        if (m_application.m_frameLoop) {
            m_application.m_frameLoop->GetMainExecutor().Stop();
        }
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
        const auto extent = m_application.m_input.FramebufferSize;
        auto& views = m_application.GetServices().Get<ViewRegistry>();
        views.SetFramebufferSize(views.GetMainView(),
                                 extent.Width > 0 ? static_cast<std::uint32_t>(extent.Width) : 0,
                                 extent.Height > 0 ? static_cast<std::uint32_t>(extent.Height) : 0);
    }
    void OnUpdate() override {
        m_application.OnUpdate();
    }

    void OnPostUpdate() override {
        m_application.OnPostUpdate();
    }

    [[nodiscard]] bool IsShutdownRequested() const override {
        return m_application.IsShutdownRequested();
    }

private:
    Application& m_application;
};

Application::Application(const ApplicationConfig& config)
    : m_hasPresentation(ValidatePresentation(config))
    , m_callbacks(std::make_unique<FrameLoopCallbacks>(*this))
    , m_windowRuntime(std::make_unique<NWindow::WindowRuntime>())
    , m_window(std::make_unique<ApplicationWindow>(*this, config.Window))
    , m_services(CreateServices())
    , m_frameLoop([this, &config] {
        (void)this;
        if (UsesGame(config)) {
            auto presentation = config.Presentation.value_or(NGraphics::PresentationConfig{});
            presentation.MaxActiveFrames = config.MaxActiveFrames;
            return std::make_unique<NRuntime::FrameLoop>(config,
                                                         GetScenes(),
                                                         GetResources(),
                                                         NRuntime::CreateGameOutput(*m_window, presentation));
        }
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
        (void)m_services->Stop();
        m_frameLoop.reset();
    }
    m_services.reset();
    m_window.reset();
    m_windowRuntime.reset();
}

void Application::Run() {
    if (std::this_thread::get_id() != m_applicationThread || m_running) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Run requires an idle Application owner thread");
    }
    m_running = true;
    try {
        (void)m_services->Start();
        m_shutdownRequested.store(m_window->ShouldClose());
        m_frameLoop->Start(
                {m_window.get(), [](void* window) noexcept {
                     NWindow::NInternal::GetWindowEngine(*static_cast<NWindow::Window*>(window)).WakeEventLoop();
                 }});
        while (!m_window->ShouldClose()) {
            RunFrame();
        }
        m_frameLoop->Stop();
        m_frameLoop->CheckError();
        if (!m_services->Stop()) {
            if (const auto failure = m_services->GetFailure()) {
                std::rethrow_exception(failure);
            }
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application services did not stop safely");
        }
    } catch (...) {
        m_frameLoop->Stop();
        (void)m_services->Stop();
        m_running = false;
        throw;
    }
    m_running = false;
}

void Application::RequestShutdown() {
    const auto close = [this] {
        m_window->RequestClose();
        m_shutdownRequested.store(true);
        if (m_frameLoop) {
            m_frameLoop->GetMainExecutor().Stop();
        }
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

ServiceManager& Application::GetServices() noexcept {
    return *m_services;
}

const ServiceManager& Application::GetServices() const noexcept {
    return *m_services;
}

NScene::SceneManager& Application::GetScenes() {
    return GetServices().Get<NScene::SceneManager>();
}

const NScene::SceneManager& Application::GetScenes() const {
    return GetServices().Get<NScene::SceneManager>();
}

NResources::ResourceManager& Application::GetResources() {
    return GetServices().Get<NResources::ResourceManager>();
}

const NResources::ResourceManager& Application::GetResources() const {
    return GetServices().Get<NResources::ResourceManager>();
}

ViewHandle Application::GetMainWindowView() const {
    return GetServices().Get<ViewRegistry>().GetMainView();
}

void Application::BindCamera(ViewHandle view, NScene::SceneHandle scene, NEcs::EntityReference camera) {
    if (std::this_thread::get_id() != m_applicationThread || m_running) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "BindCamera requires the idle Application owner");
    }
    m_services->Get<ViewRegistry>().BindCamera(view, GetScenes(), scene, camera);
}

void Application::OnUpdate() {
}

void Application::OnPostUpdate() {
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
