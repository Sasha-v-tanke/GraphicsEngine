#pragma once

#include <memory>

#include <application/application_config.h>
#include <window/window_size.h>

namespace NEngine {
class Engine;
}

namespace NWindow {
class Window;
}

namespace NApplication {

class Application {
public:
    explicit Application(const ApplicationConfig& config);

    virtual ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    void Run();

    void RequestShutdown();

    [[nodiscard]] bool IsShutdownRequested() const;

protected:
    virtual void OnUpdate();

    virtual void OnDraw();

    virtual void OnResize(NWindow::WindowSize size);

    virtual void OnFramebufferResize(NWindow::WindowSize size);

    virtual void OnClose();

    void EngineUpdateCheckpoint();

    void EngineDrawCheckpoint();

    [[nodiscard]] NWindow::Window& GetWindow() noexcept;

    [[nodiscard]] const NWindow::Window& GetWindow() const noexcept;

    [[nodiscard]] NEngine::Engine& GetEngine() noexcept;

    [[nodiscard]] const NEngine::Engine& GetEngine() const noexcept;

private:
    class ApplicationWindow;

    enum class EFrameCheckpoint {
        READY_FOR_UPDATE,
        READY_FOR_DRAW,
    };

    void RunFrame();

private:
    std::unique_ptr<NWindow::Window> m_window;
    std::unique_ptr<NEngine::Engine> m_engine;
    EFrameCheckpoint m_frameCheckpoint = EFrameCheckpoint::READY_FOR_UPDATE;
};

} // namespace NApplication
