#include <application/application_config.h>
#include <window/window_type.h>

int main() {
    NApplication::ApplicationConfig config{
            .Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    config.Window.Size = {
            .Width = 640,
            .Height = 480,
    };

    return config.Window.Size.Width == 640 && config.Window.Size.Height == 480 ? 0 : 1;
}
