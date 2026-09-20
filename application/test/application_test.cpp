#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <application/application.h>
#include <application/application_config.h>
#include <application/internal/engine_factory.h>
#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <window/internal/engine.h>
#include <window/internal/event_sink.h>
#include <window/internal/factory.h>
#include <window/window_config.h>

namespace {

using namespace std::chrono_literals;

struct FakeWindowState {
    NWindow::NInternal::IWindowEventSink* Sink = nullptr;
    void (*OnProcessEvents)(FakeWindowState& state) = nullptr;
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

        if (m_state.OnProcessEvents != nullptr) {
            m_state.OnProcessEvents(m_state);
        }

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

class BlockingFirstDrawRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    void Update(NEngine::NController::FrameScheduler& frameScheduler,
                NEngine::NController::FrameHandle frame) override {
        frameScheduler.BeginUpdate(frame);
        frameScheduler.EndUpdate(frame);
    }

    void Draw(NEngine::NController::FrameScheduler& frameScheduler, NEngine::NController::FrameHandle frame) override {
        {
            std::lock_guard lock{m_mutex};
            ++m_drawCount;
        }

        m_condition.notify_all();

        if (frame.GetFrameIndex() == 0) {
            std::unique_lock lock{m_mutex};
            m_condition.wait(lock, [this] { return m_finishFirstDraw; });
        }

        frameScheduler.BeginFinalize(frame);
        frameScheduler.CompleteFrame(frame);
        frameScheduler.RecycleFrame(frame);

        if (frame.GetFrameIndex() == 0) {
            {
                std::lock_guard lock{m_mutex};
                m_firstDrawFinished = true;
            }

            m_condition.notify_all();
        }
    }

    [[nodiscard]] bool WaitFirstDrawFinished(std::chrono::milliseconds timeout) {
        std::unique_lock lock{m_mutex};
        return m_condition.wait_for(lock, timeout, [this] { return m_firstDrawFinished; });
    }

    void FinishFirstDraw() {
        {
            std::lock_guard lock{m_mutex};
            m_finishFirstDraw = true;
        }

        m_condition.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    int m_drawCount = 0;
    bool m_finishFirstDraw = false;
    bool m_firstDrawFinished = false;
};

thread_local BlockingFirstDrawRuntime* g_blockingRuntime = nullptr;

std::unique_ptr<NEngine::Engine> CreateBlockingEngine(NEngine::EngineConfig config) {
    auto runtime = std::make_unique<BlockingFirstDrawRuntime>();
    g_blockingRuntime = runtime.get();

    return NEngine::NRuntime::EngineFactory::Create(config, std::move(runtime));
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
        NApplication::NInternal::SetEngineFactoryForTests(nullptr);
        g_fakeState = nullptr;
        g_blockingRuntime = nullptr;
    }

    FakeWindowState m_state;
};

class RecordingApplication: public NApplication::Application {
public:
    explicit RecordingApplication(const NApplication::ApplicationConfig& config, FakeWindowState& state)
        : Application(config)
        , m_state(state) {
    }

protected:
    void OnUpdate() override {
        m_state.Events.emplace_back("user.update");

        if (ThrowFromUpdate) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Injected update failure");
        }

        if (DrawCheckpointFromUpdate) {
            EngineDrawCheckpoint();
        }

        if (UpdateCheckpointFromUpdate) {
            EngineUpdateCheckpoint();
        }

        if (CloseFromUpdate) {
            RequestShutdown();
        }
    }

    void OnDraw() override {
        m_state.Events.emplace_back("user.draw");

        if (ThrowFromDraw) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Injected draw failure");
        }

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
    bool ThrowFromUpdate = false;
    bool ThrowFromDraw = false;
    bool DrawCheckpointFromUpdate = false;
    bool UpdateCheckpointFromUpdate = false;
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

    application.DrawCheckpointFromUpdate = true;

    EXPECT_THROW(application.Run(), NCommon::Exception);

    RecordingApplication secondApplication{MakeConfig(), m_state};
    secondApplication.UpdateCheckpointFromUpdate = true;

    EXPECT_THROW(secondApplication.Run(), NCommon::Exception);
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

TEST_F(ApplicationTest, DoesNotRepeatUserCallbacksWhileEngineAppliesBackpressure) {
    NApplication::NInternal::SetEngineFactoryForTests(&CreateBlockingEngine);

    NApplication::ApplicationConfig config = MakeConfig();
    config.MaxActiveFrames = 1;

    m_state.OnProcessEvents = [](FakeWindowState& state) {
        if (state.ProcessEventsCount == 4) {
            g_blockingRuntime->FinishFirstDraw();
            EXPECT_TRUE(g_blockingRuntime->WaitFirstDrawFinished(1s));
        }
    };

    class BackpressureApplication final: public RecordingApplication {
    public:
        using RecordingApplication::RecordingApplication;

    protected:
        void OnDraw() override {
            RecordingApplication::OnDraw();

            if (++m_drawCount == 2) {
                RequestShutdown();
            }
        }

    private:
        int m_drawCount = 0;
    };

    BackpressureApplication backpressureApplication{config, m_state};

    backpressureApplication.Run();

    EXPECT_GE(m_state.ProcessEventsCount, 4);
    EXPECT_EQ(m_state.Events,
              (std::vector<std::string>{
                      "window.events",
                      "user.update",
                      "user.draw",
                      "window.events",
                      "user.update",
                      "window.events",
                      "window.events",
                      "user.draw",
                      "window.request-close",
                      "user.close",
              }));
}

TEST_F(ApplicationTest, StopsEngineWhenUpdateThrowsAndCanRunAgain) {
    RecordingApplication application{MakeConfig(), m_state};
    application.ThrowFromUpdate = true;

    EXPECT_THROW(application.Run(), NCommon::Exception);

    application.ThrowFromUpdate = false;
    application.CloseFromUpdate = true;

    EXPECT_NO_THROW(application.Run());
}

TEST_F(ApplicationTest, StopsEngineWhenDrawThrowsAndCanRunAgain) {
    RecordingApplication application{MakeConfig(), m_state};
    application.ThrowFromDraw = true;

    EXPECT_THROW(application.Run(), NCommon::Exception);

    application.ThrowFromDraw = false;
    application.CloseFromUpdate = true;

    EXPECT_NO_THROW(application.Run());
}

TEST_F(ApplicationTest, DestroysWindowAfterApplicationDestruction) {
    {
        RecordingApplication application{MakeConfig(), m_state};
        application.CloseFromUpdate = true;

        application.Run();
    }

    EXPECT_EQ(m_state.DestroyedCount, 1);
    EXPECT_EQ(m_state.Events.back(), "window.destroy");
}

} // namespace
