#include <exception>
#include <iostream>

#include <application/application.h>
#include <window/engine/event_sink.h>

namespace {

struct SampleWindowState {
    int UpdateCount = 0;
    int DrawCount = 0;
    int CloseCount = 0;

    int FramesBeforeClose;
};

class SampleApplication: public NApplication::Application {
public:
    explicit SampleApplication(const NApplication::ApplicationConfig& config, SampleWindowState& state)
        : Application(config)
        , m_state(state) {
    }

protected:
    void OnUpdate() override {
        ++m_state.UpdateCount;
        std::cout << "OnUpdate calls: " << m_state.UpdateCount << std::endl;

        if (m_state.FramesBeforeClose == 0) {
            RequestShutdown();
        } else {
            --m_state.FramesBeforeClose;
        }
    }

    void OnDraw() override {
        ++m_state.DrawCount;
        std::cout << "OnDraw calls: " << m_state.DrawCount << std::endl;
    }

    void OnClose() override {
        ++m_state.CloseCount;
        std::cout << "OnClose calls: " << m_state.CloseCount << std::endl;
    }

private:
    SampleWindowState& m_state;
};

} // namespace

int main() {
    try {
        NApplication::ApplicationConfig config = {
                .Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
                .MaxActiveFrames = 2,
                .WorkerCount = 1,
        };

        SampleWindowState state{.FramesBeforeClose = 10};

        SampleApplication application{config, state};

        application.Run();

        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
