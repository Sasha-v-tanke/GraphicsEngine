#pragma once

#include <memory>
#include <string_view>

#include <window/window_config.h>
#include <window/window_size.h>

namespace NWindow {

class Window {
public:
    explicit Window(const WindowConfig& config);

    virtual ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    void SetTitle(std::string_view title);

    void SetSize(WindowSize size);

    [[nodiscard]] WindowSize GetSize() const;

    [[nodiscard]] WindowSize GetFramebufferSize() const;

    [[nodiscard]] bool ShouldClose() const;

    void RequestClose();

    void ProcessEvents();

protected:
    virtual void OnResize(WindowSize size);

    virtual void OnFramebufferResize(WindowSize size);

    virtual void OnClose();

private:
    class Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace NWindow
