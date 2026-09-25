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
#include <application/runtime/frame_loop.h>
#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>
#include <window/engine/engine.h>
#include <window/engine/event_sink.h>
#include <window/engine/factory.h>

namespace {

using namespace std::chrono_literals;

struct RuntimeWindowState {
    NWindow::NEngine::IWindowEventSink* Sink = nullptr;
    bool ShouldClose = false;
    int CreatedCount = 0;
    int DestroyedCount = 0;
    int ProcessEventsCount = 0;
    std::thread::id OwningThread;
    std::vector<std::string> Events;
};

thread_local RuntimeWindowState* g_runtimeWindowState = nullptr;

class RuntimeWindowEngine final: public NWindow::NEngine::IWindowEngine {
public:
    explicit RuntimeWindowEngine(RuntimeWindowState& state)
        : m_state(state) {
        ++m_state.CreatedCount;
        m_state.OwningThread = std::this_thread::get_id();
        m_state.Events.emplace_back("window.create");
    }

    ~RuntimeWindowEngine() override {
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
        m_state.ShouldClose = true;
        m_state.Sink->HandleClose();
    }

    void ProcessEvents() override {
        EXPECT_EQ(std::this_thread::get_id(), m_state.OwningThread);
        ++m_state.ProcessEventsCount;
        m_state.Events.emplace_back("window.events");
    }

private:
    RuntimeWindowState& m_state;
    NWindow::WindowSize m_size{};
};

std::unique_ptr<NWindow::NEngine::IWindowEngine> CreateRuntimeWindowEngine(const NWindow::WindowConfig&) {
    return std::make_unique<RuntimeWindowEngine>(*g_runtimeWindowState);
}

NApplication::ApplicationConfig MakeRuntimeConfig() {
    return {
            .Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
            .MaxActiveFrames = 2,
            .WorkerCount = 1,
    };
}

class RuntimeMvpApplication final: public NApplication::Application {
public:
    RuntimeMvpApplication(const NApplication::ApplicationConfig& config, RuntimeWindowState& state)
        : Application(config)
        , m_state(state) {
    }

private:
    void OnUpdate() override {
        EXPECT_EQ(std::this_thread::get_id(), m_state.OwningThread);
        m_state.Events.emplace_back("user.update");
        ++m_updateCount;

        if (m_updateCount == 3) {
            RequestShutdown();
        }
    }

    void OnDraw() override {
        EXPECT_EQ(std::this_thread::get_id(), m_state.OwningThread);
        m_state.Events.emplace_back("user.draw");
    }

    void OnClose() override {
        EXPECT_EQ(std::this_thread::get_id(), m_state.OwningThread);
        m_state.Events.emplace_back("user.close");
    }

private:
    RuntimeWindowState& m_state;
    int m_updateCount = 0;
};

class BlockingDrawRuntime final: public NEngine::NRuntime::IFrameRuntime {
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
};

class FinishFirstDrawGuard final {
public:
    explicit FinishFirstDrawGuard(BlockingDrawRuntime& runtime) noexcept
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
    BlockingDrawRuntime* m_runtime = nullptr;
};

class RuntimeEngineFactory final: public NApplication::NRuntime::IEngineFactory {
public:
    [[nodiscard]] std::unique_ptr<NEngine::Engine> Create(NEngine::EngineConfig config) override {
        auto runtime = std::make_unique<BlockingDrawRuntime>();
        Runtime = runtime.get();

        return NEngine::NRuntime::EngineFactory::Create(config, std::move(runtime));
    }

    BlockingDrawRuntime* Runtime = nullptr;
};

class RecordingLoopCallbacks final: public NApplication::NRuntime::IFrameLoopCallbacks {
public:
    explicit RecordingLoopCallbacks(std::vector<std::string>& events)
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

class RuntimeMvpTest: public testing::Test {
protected:
    void SetUp() override {
        g_runtimeWindowState = &m_state;
        NWindow::NEngine::SetWindowEngineFactoryForTests(&CreateRuntimeWindowEngine);
    }

    void TearDown() override {
        NWindow::NEngine::SetWindowEngineFactoryForTests(nullptr);
        g_runtimeWindowState = nullptr;
    }

    RuntimeWindowState m_state;
};

TEST_F(RuntimeMvpTest, ApplicationOwnsSingleWindowAndStopsBeforeWindowDestruction) {
    {
        RuntimeMvpApplication application{MakeRuntimeConfig(), m_state};

        application.Run();

        EXPECT_TRUE(application.IsShutdownRequested());
        EXPECT_EQ(m_state.CreatedCount, 1);
        EXPECT_EQ(m_state.DestroyedCount, 0);
    }

    ASSERT_FALSE(m_state.Events.empty());
    EXPECT_EQ(m_state.CreatedCount, 1);
    EXPECT_EQ(m_state.DestroyedCount, 1);
    EXPECT_EQ(m_state.ProcessEventsCount, 3);
    EXPECT_EQ(m_state.Events.back(), "window.destroy");
}

TEST(RuntimeMvpStress, MaxActiveFramesRingBackpressuresFastApplicationLoop) {
    NApplication::ApplicationConfig config = MakeRuntimeConfig();
    config.MaxActiveFrames = 2;

    auto engineFactory = std::make_unique<RuntimeEngineFactory>();
    RuntimeEngineFactory& engineFactoryRef = *engineFactory;
    NApplication::NRuntime::FrameLoop frameLoop{config, std::move(engineFactory)};

    std::vector<std::string> events;
    RecordingLoopCallbacks callbacks{events};

    frameLoop.Start();
    frameLoop.Step(callbacks);
    FinishFirstDrawGuard finishFirstDrawGuard{*engineFactoryRef.Runtime};
    ASSERT_TRUE(engineFactoryRef.Runtime->WaitDrawCountAtLeast(1, 2s));

    frameLoop.Step(callbacks);
    frameLoop.Step(callbacks);
    frameLoop.Step(callbacks);
    frameLoop.Step(callbacks);

    EXPECT_EQ(events,
              (std::vector<std::string>{
                      "user.update",
                      "user.draw",
                      "user.update",
                      "user.draw",
                      "user.update",
              }));

    engineFactoryRef.Runtime->FinishFirstDraw();
    finishFirstDrawGuard.Release();
    ASSERT_TRUE(engineFactoryRef.Runtime->WaitDrawCountAtLeast(2, 2s));

    frameLoop.Step(callbacks);
    ASSERT_TRUE(engineFactoryRef.Runtime->WaitDrawCountAtLeast(3, 2s));

    EXPECT_EQ(events,
              (std::vector<std::string>{
                      "user.update",
                      "user.draw",
                      "user.update",
                      "user.draw",
                      "user.update",
                      "user.draw",
              }));
}

} // namespace
