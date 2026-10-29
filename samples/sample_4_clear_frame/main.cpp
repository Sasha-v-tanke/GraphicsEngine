#include <exception>
#include <iostream>

#include <GraphicsEngine/application/application.h>

namespace {
class ClearFrameApplication final: public NApplication::Application {
public:
    ClearFrameApplication()
        : Application({
                  .Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
                  .MaxActiveFrames = 2,
                  .WorkerCount = 2,
                  .Presentation = NGraphics::PresentationConfig{},
          }) {
    }

private:
    void OnUpdate() override {
        if (GetFrameDiagnostics().Submitted >= 240) {
            RequestShutdown();
        }
    }
};
} // namespace

int main() {
    try {
        ClearFrameApplication application;
        application.Run();
        const auto diagnostics = application.GetFrameDiagnostics();
        std::cout << "submitted=" << diagnostics.Submitted << " completed=" << diagnostics.Completed
                  << " recycled=" << diagnostics.Recycled << '\n';
        return diagnostics.Submitted == diagnostics.Completed && diagnostics.Completed == diagnostics.Recycled ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
