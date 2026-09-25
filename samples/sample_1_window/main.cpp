#include <exception>
#include <iostream>

#include <GraphicsEngine/window/window.h>
#include <GraphicsEngine/window/window_runtime.h>

namespace {

class SampleWindow final: public NWindow::Window {
public:
    using Window::Window;

protected:
    void OnResize(NWindow::WindowSize size) override {
        std::cout << "Window resized: " << size.Width << "x" << size.Height << '\n';
    }

    void OnFramebufferResize(NWindow::WindowSize size) override {
        std::cout << "Framebuffer resized: " << size.Width << "x" << size.Height << '\n';
    }

    void OnClose() override {
        std::cout << "Window close requested\n";
    }
};

} // namespace

int main() {
    try {
        NWindow::WindowConfig config{NWindow::EWindowType::GLFW};

        config.Title = "GraphicsEngine GLFW Window";
        config.Size = {
                .Width = 1280,
                .Height = 720,
        };

        NWindow::WindowRuntime windowRuntime;
        SampleWindow window{config};

        while (!window.ShouldClose()) {
            window.ProcessEvents();
        }

        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
