#pragma once

#include <memory>

#include <application/application_config.h>
#include <window/window_size.h>

namespace NWindow {
class Window;
}

namespace NApplication::NRuntime {
class FrameLoop;
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

    [[nodiscard]] NWindow::Window& GetWindow() noexcept;

    [[nodiscard]] const NWindow::Window& GetWindow() const noexcept;

private:
    class ApplicationWindow;
    class FrameLoopCallbacks;

    void RunFrame();

private:
    std::unique_ptr<NWindow::Window> m_window;
    std::unique_ptr<NRuntime::FrameLoop> m_frameLoop;
};

} // namespace NApplication
