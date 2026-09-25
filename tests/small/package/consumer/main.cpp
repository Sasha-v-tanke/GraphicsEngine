#include <GraphicsEngine/application/application_config.h>
#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/window/window_runtime.h>
#include <GraphicsEngine/window/window_type.h>

int main() {
    NWindow::WindowRuntime runtime;

    NApplication::ApplicationConfig config{
            .Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    config.Window.Size = {
            .Width = 640,
            .Height = 480,
    };

    const std::error_code error = NCommon::make_error_code(NCommon::EError::INVALID_STATE);

    if (!error) {
        return 1;
    }

    return config.Window.Size.Width == 640 && config.Window.Size.Height == 480 ? 0 : 1;
}
