#pragma once

#include <memory>

#include <GraphicsEngine/application/application_config.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/window/window_size.h>

namespace NWindow {

class Window;
class WindowRuntime;

} // namespace NWindow

namespace NApplication::NRuntime {

class FrameLoop;

}

namespace NApplication {

class Application: public NCommon::NonTransferable {
public:
    explicit Application(const ApplicationConfig& config);

    virtual ~Application();

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
    std::unique_ptr<NWindow::WindowRuntime> m_windowRuntime;
    std::unique_ptr<NWindow::Window> m_window;
    std::unique_ptr<NRuntime::FrameLoop> m_frameLoop;
};

} // namespace NApplication
