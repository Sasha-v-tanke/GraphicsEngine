#pragma once

#include <memory>
#include <string_view>

#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/window/window_config.h>
#include <GraphicsEngine/window/window_size.h>

namespace NWindow {

// Framework-independent window facade. The object has thread affinity to the thread that constructs it.
class Window: public NCommon::NonTransferable {
public:
    // For GLFW windows, a WindowRuntime must already be active on the application/process main thread.
    explicit Window(const WindowConfig& config);

    // Must run on the owning thread. Concrete backends may terminate on wrong-thread destruction.
    virtual ~Window();

    // All operations must run on the owning thread. Invalid-thread use throws when the backend can report it.
    void SetTitle(std::string_view title);

    void SetSize(WindowSize size);

    [[nodiscard]] WindowSize GetSize() const;

    [[nodiscard]] WindowSize GetFramebufferSize() const;

    [[nodiscard]] bool ShouldClose() const;

    // Marks the window for closing and emits OnClose once if the close state changes.
    void RequestClose();

    // Processes backend events and then dispatches queued callbacks on the owning thread.
    void ProcessEvents();

protected:
    // Callbacks run during ProcessEvents or RequestClose on the owning thread.
    // Backend callbacks are not forwarded reentrantly from native callback frames.
    virtual void OnResize(WindowSize size);

    virtual void OnFramebufferResize(WindowSize size);

    virtual void OnClose();

private:
    class Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace NWindow
