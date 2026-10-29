#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <application/application.h>
#include <application/application_config.h>
#include <application/runtime/application_access.h>
#include <application/runtime/frame_loop.h>
#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <window/engine/engine.h>
#include <window/engine/event_sink.h>
#include <window/engine/factory.h>
#include <window/window_config.h>

namespace {

using namespace std::chrono_literals;

struct FakeWindowState {
    NWindow::NEngine::IWindowEventSink* Sink = nullptr;
    void (*OnProcessEvents)(FakeWindowState& state) = nullptr;
    std::function<void()> EventAction;
    bool ShouldClose = false;
    bool EmitCloseOnNextProcessEvents = false;
    int ProcessEventsCount = 0;
    int RequestCloseCount = 0;
    int DestroyedCount = 0;
    std::vector<std::string> Events;
};

thread_local FakeWindowState* g_fakeState = nullptr;

class FakeWindowEngine final: public NWindow::NEngine::IWindowEngine {
public:
    explicit FakeWindowEngine(FakeWindowState& state)
        : m_state(state) {
    }

    ~FakeWindowEngine() override {
        m_state.Events.emplace_back("window.destroy");
        ++m_state.DestroyedCount;
    }

    void AttachEventSink(NWindow::NEngine::IWindowEventSink& eventSink) override {
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

        if (m_state.EventAction) {
            m_state.EventAction();
        }
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

std::unique_ptr<NWindow::NEngine::IWindowEngine> CreateFakeWindowEngine(const NWindow::WindowConfig&) {
    return std::make_unique<FakeWindowEngine>(*g_fakeState);
}

class BlockingFirstDrawRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    void Update(const NEngine::NRuntime::FrameContext&) override {
    }

    void Draw(const NEngine::NRuntime::FrameContext& frame) override {
        {
            std::lock_guard lock{m_mutex};
            ++m_drawCount;
        }

        m_condition.notify_all();

        if (frame.GetFrame().GetFrameIndex() == 0) {
            std::unique_lock lock{m_mutex};
            m_condition.wait(lock, [this] { return m_finishFirstDraw; });
        }

        if (frame.GetFrame().GetFrameIndex() == 0) {
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

    [[nodiscard]] bool WaitDrawCountAtLeast(int drawCount, std::chrono::milliseconds timeout) {
        std::unique_lock lock{m_mutex};
        return m_condition.wait_for(lock, timeout, [this, drawCount] { return m_drawCount >= drawCount; });
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

class FinishFirstDrawGuard final {
public:
    explicit FinishFirstDrawGuard(BlockingFirstDrawRuntime& runtime) noexcept
        : m_runtime(&runtime) {
    }

    ~FinishFirstDrawGuard() {
        if (m_runtime != nullptr) {
            m_runtime->FinishFirstDraw();
        }
    }

    void Release() noexcept {
        m_runtime = nullptr;
    }

private:
    BlockingFirstDrawRuntime* m_runtime = nullptr;
};

class BlockingEngineFactory final: public NApplication::NRuntime::IEngineFactory {
public:
    [[nodiscard]] std::unique_ptr<NEngine::Engine> Create(NEngine::EngineConfig config) override {
        auto runtime = std::make_unique<BlockingFirstDrawRuntime>();
        Runtime = runtime.get();

        return NEngine::NRuntime::EngineFactory::Create(config, std::move(runtime));
    }

    BlockingFirstDrawRuntime* Runtime = nullptr;
};

class RecordingFrameLoopCallbacks final: public NApplication::NRuntime::IFrameLoopCallbacks {
public:
    explicit RecordingFrameLoopCallbacks(std::vector<std::string>& events)
        : m_events(events) {
    }

    void OnUpdate() override {
        m_events.emplace_back("user.update");
    }

    void OnDraw() override {
        m_events.emplace_back("user.draw");
    }

private:
    std::vector<std::string>& m_events;
};

class FailingAsyncExtractionRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    void Update(const NEngine::NRuntime::FrameContext&) override {
        ++Updates;
    }

    NCommon::TaskHandle ScheduleExtract(const NEngine::NRuntime::FrameContext&, NCommon::TaskContext& task) override {
        const NCommon::TaskHandle dependencies[] = {task.GetTask()};
        return task.Spawn([](NCommon::TaskContext&) { throw std::runtime_error{"Async extraction failed"}; },
                          dependencies);
    }

    void Draw(const NEngine::NRuntime::FrameContext&) override {
        ++Draws;
    }

    std::atomic<int> Updates{0};
    std::atomic<int> Draws{0};
};

class FailingAsyncExtractionFactory final: public NApplication::NRuntime::IEngineFactory {
public:
    std::unique_ptr<NEngine::Engine> Create(NEngine::EngineConfig config) override {
        auto runtime = std::make_unique<FailingAsyncExtractionRuntime>();
        Runtime = runtime.get();
        auto engine = NEngine::NRuntime::EngineFactory::Create(config, std::move(runtime));
        Engine = engine.get();
        return engine;
    }

    NEngine::Engine* Engine = nullptr;
    FailingAsyncExtractionRuntime* Runtime = nullptr;
};

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
        NWindow::NEngine::SetWindowEngineFactoryForTests(&CreateFakeWindowEngine);
    }

    void TearDown() override {
        NWindow::NEngine::SetWindowEngineFactoryForTests(nullptr);
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

protected:
    void OnUpdate() override {
        m_state.Events.emplace_back("user.update");

        if (ThrowFromUpdate) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Injected update failure");
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
    NApplication::NRuntime::FrameLoop frameLoop{MakeConfig()};

    frameLoop.Start();
    EXPECT_THROW(frameLoop.EngineDrawCheckpoint(), NCommon::Exception);
    EXPECT_THROW(frameLoop.EngineUpdateCheckpoint(), NCommon::Exception);
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
    NApplication::ApplicationConfig config = MakeConfig();
    config.MaxActiveFrames = 1;

    auto engineFactory = std::make_unique<BlockingEngineFactory>();
    BlockingEngineFactory& blockingEngineFactory = *engineFactory;

    NApplication::NRuntime::FrameLoop frameLoop{config, std::move(engineFactory)};
    RecordingFrameLoopCallbacks callbacks{m_state.Events};

    frameLoop.Start();
    frameLoop.Step(callbacks);
    FinishFirstDrawGuard finishFirstDrawGuard{*blockingEngineFactory.Runtime};
    ASSERT_TRUE(blockingEngineFactory.Runtime->WaitDrawCountAtLeast(1, 1s));
    frameLoop.Step(callbacks);
    frameLoop.Step(callbacks);
    bool mainCommandRan = false;
    auto& executor = frameLoop.GetMainExecutor();
    auto completion = executor.Post(executor.GetGeneration(), [&] { mainCommandRan = true; });
    frameLoop.Step(callbacks);
    EXPECT_TRUE(mainCommandRan);
    EXPECT_EQ(completion.Wait(), NApplication::NRuntime::EMainCommandState::COMPLETED);

    EXPECT_EQ(m_state.Events,
              (std::vector<std::string>{
                      "user.update",
                      "user.draw",
                      "user.update",
              }));

    blockingEngineFactory.Runtime->FinishFirstDraw();
    finishFirstDrawGuard.Release();
    EXPECT_TRUE(blockingEngineFactory.Runtime->WaitFirstDrawFinished(1s));

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (m_state.Events.size() < 4 && std::chrono::steady_clock::now() < deadline) {
        frameLoop.Step(callbacks);
        std::this_thread::yield();
    }

    EXPECT_EQ(m_state.Events,
              (std::vector<std::string>{
                      "user.update",
                      "user.draw",
                      "user.update",
                      "user.draw",
              }));
}

TEST_F(ApplicationTest, ObservesAsyncExtractionFailureWhileUpdateAdmissionIsClosed) {
    auto factory = std::make_unique<FailingAsyncExtractionFactory>();
    auto& factoryRef = *factory;
    NApplication::NRuntime::FrameLoop frameLoop{MakeConfig(), std::move(factory)};
    RecordingFrameLoopCallbacks callbacks{m_state.Events};
    frameLoop.Start();

    bool failed = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!failed && std::chrono::steady_clock::now() < deadline) {
        try {
            frameLoop.Step(callbacks);
        } catch (const NCommon::Exception&) {
            failed = true;
        }
        std::this_thread::yield();
    }

    EXPECT_TRUE(failed);
    EXPECT_EQ(factoryRef.Engine->GetState(), NEngine::EEngineState::STOPPING);
    ASSERT_TRUE(factoryRef.Engine->GetLastError().has_value());
    EXPECT_EQ(factoryRef.Engine->GetLastError()->Message, "Async extraction failed");
    EXPECT_EQ(factoryRef.Runtime->Updates.load(), 1);
    EXPECT_EQ(factoryRef.Runtime->Draws.load(), 0);
    EXPECT_EQ(m_state.Events, (std::vector<std::string>{"user.update", "user.draw"}));
}

TEST_F(ApplicationTest, FastApplicationGetsBackpressureFromSlowWorkers) {
    NApplication::ApplicationConfig config = MakeConfig();
    config.MaxActiveFrames = 2;

    auto engineFactory = std::make_unique<BlockingEngineFactory>();
    BlockingEngineFactory& blockingEngineFactory = *engineFactory;

    NApplication::NRuntime::FrameLoop frameLoop{config, std::move(engineFactory)};
    RecordingFrameLoopCallbacks callbacks{m_state.Events};

    frameLoop.Start();
    frameLoop.Step(callbacks);
    FinishFirstDrawGuard finishFirstDrawGuard{*blockingEngineFactory.Runtime};
    ASSERT_TRUE(blockingEngineFactory.Runtime->WaitDrawCountAtLeast(1, 1s));

    frameLoop.Step(callbacks);
    frameLoop.Step(callbacks);

    EXPECT_EQ(m_state.Events,
              (std::vector<std::string>{
                      "user.update",
                      "user.draw",
                      "user.update",
                      "user.draw",
              }));

    blockingEngineFactory.Runtime->FinishFirstDraw();
    finishFirstDrawGuard.Release();
    EXPECT_TRUE(blockingEngineFactory.Runtime->WaitFirstDrawFinished(1s));
    EXPECT_TRUE(blockingEngineFactory.Runtime->WaitDrawCountAtLeast(2, 1s));

    frameLoop.Step(callbacks);

    EXPECT_EQ(m_state.Events,
              (std::vector<std::string>{
                      "user.update",
                      "user.draw",
                      "user.update",
                      "user.draw",
                      "user.update",
                      "user.draw",
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

TEST_F(ApplicationTest, MainCommandClosePrecedesFrameCallbacksAndSkipsPendingAcrossRestart) {
    using NApplication::NRuntime::EMainCommandState;
    using NApplication::NRuntime::MainCompletion;
    RecordingApplication application{MakeConfig(), m_state};
    auto& executor = NApplication::NRuntime::ApplicationAccess::GetMainExecutor(application);
    std::optional<MainCompletion> close;
    std::optional<MainCompletion> pending;
    m_state.EventAction = [&] {
        close = executor.Post(executor.GetGeneration(), [&] { application.RequestShutdown(); });
        pending = executor.Post(executor.GetGeneration(), [] { ADD_FAILURE() << "Pending command ran after close"; });
    };
    application.Run();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->Wait(), EMainCommandState::COMPLETED);
    EXPECT_EQ(pending->Wait(), EMainCommandState::SKIPPED);
    EXPECT_EQ(m_state.Events, (std::vector<std::string>{"window.events", "window.request-close", "user.close"}));
    const auto previous = executor.GetGeneration();
    m_state.ShouldClose = false;
    m_state.EventAction = [&] {
        auto stale = executor.Post(previous, [] { ADD_FAILURE() << "Previous Run command ran"; });
        EXPECT_EQ(stale.Wait(), EMainCommandState::SKIPPED);
        close = executor.Post(executor.GetGeneration(), [&] { application.RequestShutdown(); });
    };
    application.Run();
    EXPECT_GT(executor.GetGeneration(), previous);
    EXPECT_EQ(close->Wait(), EMainCommandState::COMPLETED);
}

TEST_F(ApplicationTest, EventWaitIsWokenByProducerWithoutDispatchingOnProducerThread) {
    FakeWindowEngine window{m_state};
    std::thread producer{[&] { window.WakeEventLoop(); }};
    producer.join();
    const auto start = std::chrono::steady_clock::now();
    window.WaitEvents(2s);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    EXPECT_EQ(m_state.ProcessEventsCount, 1);
}

class MainWaitingRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    NApplication::NRuntime::MainExecutor* Executor = nullptr;
    std::promise<void> Queued;
    std::atomic<bool> Skipped{false};
    void Update(const NEngine::NRuntime::FrameContext&) override {
    }
    void Draw(const NEngine::NRuntime::FrameContext&) override {
        auto command = Executor->Post(Executor->GetGeneration(), [] { ADD_FAILURE(); });
        Queued.set_value();
        Skipped = command.Wait() == NApplication::NRuntime::EMainCommandState::SKIPPED;
    }
};

class MainWaitingFactory final: public NApplication::NRuntime::IEngineFactory {
public:
    MainWaitingRuntime* Runtime = nullptr;
    std::unique_ptr<NEngine::Engine> Create(NEngine::EngineConfig config) override {
        auto runtime = std::make_unique<MainWaitingRuntime>();
        Runtime = runtime.get();
        return NEngine::NRuntime::EngineFactory::Create(config, std::move(runtime));
    }
};

TEST_F(ApplicationTest, StopSkipsMainCommandsBeforeJoiningSingleWorker) {
    auto factory = std::make_unique<MainWaitingFactory>();
    MainWaitingFactory& reference = *factory;
    NApplication::NRuntime::FrameLoop loop{MakeConfig(), std::move(factory)};
    reference.Runtime->Executor = &loop.GetMainExecutor();
    auto queued = reference.Runtime->Queued.get_future();
    RecordingFrameLoopCallbacks callbacks{m_state.Events};
    loop.Start();
    loop.Step(callbacks);
    EXPECT_EQ(queued.wait_for(2s), std::future_status::ready);
    loop.Stop();
    EXPECT_TRUE(reference.Runtime->Skipped.load());
}

TEST_F(ApplicationTest, WindowCloseIsProcessedBetweenBoundedMainBatches) {
    using NApplication::NRuntime::ApplicationAccess;
    using NApplication::NRuntime::EMainCommandState;
    using NApplication::NRuntime::MainCompletion;
    using NApplication::NRuntime::MainExecutor;
    RecordingApplication application{MakeConfig(), m_state};
    MainExecutor& executor = ApplicationAccess::GetMainExecutor(application);
    std::vector<MainCompletion> commands;
    std::size_t ran = 0;
    m_state.EventAction = [&] {
        if (m_state.ProcessEventsCount == 1) {
            for (std::size_t index = 0; index < MainExecutor::DISPATCH_BATCH + 5; ++index) {
                commands.push_back(executor.Post(executor.GetGeneration(), [&] { ++ran; }));
            }
        } else {
            m_state.EmitCloseOnNextProcessEvents = true;
        }
    };
    application.Run();
    EXPECT_EQ(ran, MainExecutor::DISPATCH_BATCH);
    EXPECT_EQ(commands.back().Wait(), EMainCommandState::SKIPPED);
    EXPECT_EQ(m_state.ProcessEventsCount, 2);
}

} // namespace
