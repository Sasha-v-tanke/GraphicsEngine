#include <exception>
#include <iostream>

#include <GraphicsEngine/application/application.h>

namespace {

class RuntimeMvpApplication final: public NApplication::Application {
public:
    RuntimeMvpApplication()
        : Application({
                  .Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
                  .MaxActiveFrames = 2,
                  .WorkerCount = 2,
          }) {
    }

private:
    void OnUpdate() override {
        ++m_updateCount;
        std::cout << "update " << m_updateCount << '\n';

        if (m_updateCount >= 120) {
            RequestShutdown();
        }
    }

    void OnDraw() override {
        ++m_drawCount;
        std::cout << "draw " << m_drawCount << '\n';
    }

    void OnClose() override {
        std::cout << "close" << '\n';
    }

private:
    int m_updateCount = 0;
    int m_drawCount = 0;
};

} // namespace

int main() {
    try {
        RuntimeMvpApplication application;
        application.Run();

        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
