#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <application/application.h>
#include <application/application_config.h>
#include <engine/engine.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <window/internal/engine.h>
#include <window/internal/event_sink.h>
#include <window/internal/factory.h>
#include <window/window_config.h>

namespace {

struct FakeWindowState {
    NWindow::NInternal::IWindowEventSink* Sink = nullptr;
    bool ShouldClose = false;
    bool EmitCloseOnNextProcessEvents = false;
    int ProcessEventsCount = 0;
    int RequestCloseCount = 0;
    int DestroyedCount = 0;
    std::vector<std::string> Events;
};

thread_local FakeWindowState* g_fakeState = nullptr;

class FakeWindowEngine final: public NWindow::NInternal::IWindowEngine {
public:
    explicit FakeWindowEngine(FakeWindowState& state)
        : m_state(state) {
    }

    ~FakeWindowEngine() override {
        m_state.Events.emplace_back("window.destroy");
        ++m_state.DestroyedCount;
    }

    void AttachEventSink(NWindow::NInternal::IWindowEventSink& eventSink) override {
        m_state.Sink = &eventSink;
    }

    void SetTitle(std::string_view) override {
    }

    void SetSize(NWindow::WindowSize size) override {
        m_size = size;
    }

    [[nodiscard]] NWindow::WindowSize GetSize() const override {
        return m_size;
    }

    [[nodiscard]] NWindow::WindowSize GetFramebufferSize() const override {
        return m_size;
    }

    [[nodiscard]] bool ShouldClose() const override {
        return m_state.ShouldClose;
    }

    void RequestClose() override {
        m_state.Events.emplace_back("window.request-close");
        ++m_state.RequestCloseCount;
        m_state.ShouldClose = true;
    }

    void ProcessEvents() override {
        m_state.Events.emplace_back("window.events");
        ++m_state.ProcessEventsCount;

        if (m_state.EmitCloseOnNextProcessEvents) {
            m_state.EmitCloseOnNextProcessEvents = false;
            m_state.ShouldClose = true;
            m_state.Sink->HandleClose();
        }
    }

private:
    FakeWindowState& m_state;
    NWindow::WindowSize m_size{};
};

std::unique_ptr<NWindow::NInternal::IWindowEngine> CreateFakeWindowEngine(const NWindow::WindowConfig&) {
    return std::make_unique<FakeWindowEngine>(*g_fakeState);
}

NApplication::ApplicationConfig MakeConfig() {
    return {
            .Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
            .MaxActiveFrames = 2,
            .WorkerCount = 1,
    };
}

class ApplicationTest: public testing::Test {
protected:
    void SetUp() override {
        g_fakeState = &m_state;
        NWindow::NInternal::SetWindowEngineFactoryForTests(&CreateFakeWindowEngine);
    }

    void TearDown() override {
        NWindow::NInternal::SetWindowEngineFactoryForTests(nullptr);
        g_fakeState = nullptr;
    }

    FakeWindowState m_state;
};

class RecordingApplication: public NApplication::Application {
public:
    explicit RecordingApplication(const NApplication::ApplicationConfig& config, FakeWindowState& state)
        : Application(config)
        , m_state(state) {
    }

    void UpdateCheckpoint() {
        EngineUpdateCheckpoint();
    }

    void DrawCheckpoint() {
        EngineDrawCheckpoint();
    }

    void StartEngine() {
        GetEngine().Start();
    }

    [[nodiscard]] NEngine::EEngineState GetEngineState() const {
        return GetEngine().GetState();
    }

protected:
    void OnUpdate() override {
        m_state.Events.emplace_back("user.update");

        if (CloseFromUpdate) {
            RequestShutdown();
        }
    }

    void OnDraw() override {
        m_state.Events.emplace_back("user.draw");

        if (CloseFromDraw) {
            RequestShutdown();
        }
    }

    void OnClose() override {
        m_state.Events.emplace_back("user.close");

        if (CloseFromCallback) {
            RequestShutdown();
        }
    }

private:
    FakeWindowState& m_state;

public:
    bool CloseFromUpdate = false;
    bool CloseFromDraw = false;
    bool CloseFromCallback = false;
};

TEST_F(ApplicationTest, RunProcessesFramesUntilClose) {
    RecordingApplication application{MakeConfig(), m_state};
    application.CloseFromUpdate = true;

    application.Run();

    EXPECT_EQ(m_state.RequestCloseCount, 1);
    EXPECT_EQ(m_state.ProcessEventsCount, 1);
    EXPECT_EQ(m_state.Events,
              (std::vector<std::string>{
                      "window.events",
                      "user.update",
                      "window.request-close",
                      "user.close",
                      "user.draw",
              }));
}

TEST_F(ApplicationTest, RunsCallbacksAndFrameCheckpointsInOrderOnApplicationThread) {
    const std::thread::id applicationThread = std::this_thread::get_id();

    class ThreadCheckingApplication final: public RecordingApplication {
    public:
        ThreadCheckingApplication(const NApplication::ApplicationConfig& config,
                                  FakeWindowState& state,
                                  std::thread::id expectedThread)
            : RecordingApplication(config, state)
            , m_expectedThread(expectedThread) {
        }

    protected:
        void OnUpdate() override {
            EXPECT_EQ(std::this_thread::get_id(), m_expectedThread);
            RecordingApplication::OnUpdate();
        }

        void OnDraw() override {
            EXPECT_EQ(std::this_thread::get_id(), m_expectedThread);
            RecordingApplication::OnDraw();
        }

    private:
        std::thread::id m_expectedThread;
    };

    ThreadCheckingApplication application{MakeConfig(), m_state, applicationThread};
    application.CloseFromDraw = true;

    application.Run();

    EXPECT_EQ(m_state.Events,
              (std::vector<std::string>{
                      "window.events",
                      "user.update",
                      "user.draw",
                      "window.request-close",
                      "user.close",
              }));
}

TEST_F(ApplicationTest, RejectsInvalidCheckpointOrder) {
    RecordingApplication application{MakeConfig(), m_state};

    application.StartEngine();

    EXPECT_THROW(application.DrawCheckpoint(), NCommon::Exception);

    application.UpdateCheckpoint();

    EXPECT_THROW(application.UpdateCheckpoint(), NCommon::Exception);
}

TEST_F(ApplicationTest, AllowsShutdownRequestFromWindowCallback) {
    RecordingApplication application{MakeConfig(), m_state};
    application.CloseFromCallback = true;

    m_state.EmitCloseOnNextProcessEvents = true;

    application.Run();

    EXPECT_EQ(m_state.RequestCloseCount, 0);
    EXPECT_EQ(m_state.Events,
              (std::vector<std::string>{
                      "window.events",
                      "user.close",
              }));
}

TEST_F(ApplicationTest, StopsEngineBeforeDestroyingWindow) {
    {
        RecordingApplication application{MakeConfig(), m_state};
        application.StartEngine();

        EXPECT_EQ(application.GetEngineState(), NEngine::EEngineState::RUNNING);
    }

    EXPECT_EQ(m_state.DestroyedCount, 1);
    EXPECT_EQ(m_state.Events.back(), "window.destroy");
}

} // namespace
