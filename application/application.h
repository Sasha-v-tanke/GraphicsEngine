#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <GraphicsEngine/application/application_config.h>
#include <GraphicsEngine/application/frame_diagnostics.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/lib/thread/job.h>
#include <GraphicsEngine/window/window_size.h>

namespace NWindow {

class Window;
class WindowRuntime;

} // namespace NWindow

namespace NApplication::NRuntime {

class FrameLoop;

}

namespace NApplication {

// Owns Window and Engine runtime subsystems while the caller owns the outer main loop thread.
class Application: public NCommon::NonTransferable {
public:
    // Constructs Window first, then frame runtime. Throws if the configured window/runtime cannot be created.
    explicit Application(const ApplicationConfig& config);

    // Stops Engine work before destroying Window. Must run on the same application thread that created the object.
    virtual ~Application();

    // Runs until shutdown is requested. It may be called again after returning; re-entering while active is invalid.
    // Exceptions from callbacks or runtime checkpoints stop Engine work and propagate to the caller.
    void Run();

    // Requests close through the owned Window. OnClose is emitted by Window close semantics, not by this call directly.
    void RequestShutdown();

    [[nodiscard]] FrameDiagnostics GetFrameDiagnostics() const;

    [[nodiscard]] bool IsShutdownRequested() const;

protected:
    // Starts frame-independent work on TaskSystem. No implicit World/frame access or main-thread affinity.
    // Available during Run only; submission failure throws synchronously. TryTake never waits.
    // Stop drains accepted work. Captures must remain valid until completion; dropping Job does not cancel.
    // Jobs share worker CPU with frames and must not block waiting for work on the same pool.
    template<NCommon::UserJobFunction Function>
    [[nodiscard]] auto RunAsync(Function&& function) {
        return NCommon::NJobInternal::JobFactory::Run(
                [this](std::function<void()> task) { SubmitUserJob(std::move(task)); },
                std::forward<Function>(function));
    }

    // Called on the application thread, never on TaskSystem workers.
    virtual void OnUpdate();

    // Called on the application thread after the engine update checkpoint for the current frame is ready.
    virtual void OnDraw();

    // Window callbacks are delivered on the application thread during event processing.
    virtual void OnResize(NWindow::WindowSize size);

    virtual void OnFramebufferResize(NWindow::WindowSize size);

    virtual void OnClose();

    [[nodiscard]] NWindow::Window& GetWindow() noexcept;

    [[nodiscard]] const NWindow::Window& GetWindow() const noexcept;

private:
    class ApplicationWindow;
    class FrameLoopCallbacks;

    void RunFrame();

    void SubmitUserJob(std::function<void()> function);

private:
    bool m_hasPresentation = false;
    std::unique_ptr<NWindow::WindowRuntime> m_windowRuntime;
    std::unique_ptr<NWindow::Window> m_window;
    std::unique_ptr<NRuntime::FrameLoop> m_frameLoop;
};

} // namespace NApplication
