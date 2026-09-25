#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>
#include <window/engine/application_thread.h>
#include <window/engine/engine.h>
#include <window/engine/event_queue.h>
#include <window/engine/event_sink.h>
#include <window/engine/factory.h>
#include <window/window.h>
#include <window/window_config.h>
#include <window/window_runtime.h>
#include <window/window_size.h>
#include <window/window_type.h>

namespace {

struct FakeWindowState {
    NWindow::EWindowType ConfigType = NWindow::EWindowType::GLFW;
    std::string ConfigTitle;
    NWindow::WindowSize ConfigSize{};

    std::string Title;
    NWindow::WindowSize Size{};
    NWindow::WindowSize FramebufferSize{};

    bool ShouldClose = false;

    int CreatedCount = 0;
    int DestroyedCount = 0;
    int ProcessEventsCount = 0;
    int RequestCloseCount = 0;
};

thread_local FakeWindowState* g_fakeState = nullptr;

class FakeWindowEngine final: public NWindow::NEngine::IWindowEngine {
public:
    explicit FakeWindowEngine(FakeWindowState& state)
        : m_state(state) {
        ++m_state.CreatedCount;
    }

    ~FakeWindowEngine() override {
        ++m_state.DestroyedCount;
    }

    void AttachEventSink(NWindow::NEngine::IWindowEventSink& eventSink) override {
        m_eventSink = &eventSink;
    }

    void SetTitle(std::string_view title) override {
        m_state.Title = title;
    }

    void SetSize(NWindow::WindowSize size) override {
        m_state.Size = size;
    }

    [[nodiscard]] NWindow::WindowSize GetSize() const override {
        return m_state.Size;
    }

    [[nodiscard]] NWindow::WindowSize GetFramebufferSize() const override {
        return m_state.FramebufferSize;
    }

    [[nodiscard]] bool ShouldClose() const override {
        return m_state.ShouldClose;
    }

    void RequestClose() override {
        ++m_state.RequestCloseCount;

        m_state.ShouldClose = true;
    }

    void ProcessEvents() override {
        ++m_state.ProcessEventsCount;
    }

    void EmitResize(NWindow::WindowSize size) {
        ASSERT_NE(m_eventSink, nullptr);

        m_state.Size = size;

        m_eventSink->HandleResize(size);
    }

    void EmitFramebufferResize(NWindow::WindowSize size) {
        ASSERT_NE(m_eventSink, nullptr);

        m_state.FramebufferSize = size;

        m_eventSink->HandleFramebufferResize(size);
    }

    void EmitClose() {
        ASSERT_NE(m_eventSink, nullptr);

        m_state.ShouldClose = true;

        m_eventSink->HandleClose();
    }

    [[nodiscard]] bool HasEventSink() const {
        return m_eventSink != nullptr;
    }

private:
    NWindow::NEngine::IWindowEventSink* m_eventSink = nullptr;

    FakeWindowState& m_state;
};

thread_local FakeWindowEngine* g_fakeEngine = nullptr;

std::unique_ptr<NWindow::NEngine::IWindowEngine> CreateFakeWindowEngine(const NWindow::WindowConfig& config) {
    g_fakeState->ConfigType = config.Type;
    g_fakeState->ConfigTitle = config.Title;
    g_fakeState->ConfigSize = config.Size;

    g_fakeState->Title = config.Title;
    g_fakeState->Size = config.Size;
    g_fakeState->FramebufferSize = config.Size;

    std::unique_ptr<FakeWindowEngine> engine = std::make_unique<FakeWindowEngine>(*g_fakeState);

    g_fakeEngine = engine.get();

    return engine;
}

std::unique_ptr<NWindow::NEngine::IWindowEngine> CreateNullWindowEngine(const NWindow::WindowConfig&) {
    return nullptr;
}

class TestWindow final: public NWindow::Window {
public:
    using Window::Window;

    int ResizeCount = 0;
    int FramebufferResizeCount = 0;
    int CloseCount = 0;

    NWindow::WindowSize LastSize{};
    NWindow::WindowSize LastFramebufferSize{};

protected:
    void OnResize(NWindow::WindowSize size) override {
        ++ResizeCount;

        LastSize = size;
    }

    void OnFramebufferResize(NWindow::WindowSize size) override {
        ++FramebufferResizeCount;

        LastFramebufferSize = size;
    }

    void OnClose() override {
        ++CloseCount;
    }
};

class WindowTest: public testing::Test {
protected:
    void SetUp() override {
        m_runtime = std::make_unique<NWindow::WindowRuntime>();

        g_fakeState = &m_state;
        g_fakeEngine = nullptr;

        NWindow::NEngine::SetWindowEngineFactoryForTests(&CreateFakeWindowEngine);
    }

    void TearDown() override {
        NWindow::NEngine::SetWindowEngineFactoryForTests(nullptr);

        g_fakeEngine = nullptr;
        g_fakeState = nullptr;

        m_runtime.reset();
    }

    std::unique_ptr<NWindow::WindowRuntime> m_runtime;
    FakeWindowState m_state;
};

class RecordingEventSink final: public NWindow::NEngine::IWindowEventSink {
public:
    void HandleResize(NWindow::WindowSize size) override {
        Events.emplace_back("resize");
        LastWindowSize = size;
    }

    void HandleFramebufferResize(NWindow::WindowSize size) override {
        Events.emplace_back("framebuffer");
        LastFramebufferSize = size;
    }

    void HandleClose() override {
        Events.emplace_back("close");
    }

    std::vector<std::string> Events;
    NWindow::WindowSize LastWindowSize{};
    NWindow::WindowSize LastFramebufferSize{};
};

static_assert(!std::is_copy_constructible_v<NWindow::Window>);
static_assert(!std::is_copy_assignable_v<NWindow::Window>);
static_assert(!std::is_move_constructible_v<NWindow::Window>);
static_assert(!std::is_move_assignable_v<NWindow::Window>);

// -----------------------------------------------------------------------------
// Runtime thread affinity
// -----------------------------------------------------------------------------

TEST(WindowRuntime, AllowsRegisteredApplicationThread) {
    NWindow::WindowRuntime runtime;

    EXPECT_NO_THROW(NWindow::NEngine::ValidateApplicationThread("test window operation"));
}

TEST(WindowRuntime, RejectsAccessFromAnotherThread) {
    NWindow::WindowRuntime runtime;

    bool rejected = false;

    std::thread thread{[&rejected] {
        try {
            NWindow::NEngine::ValidateApplicationThread("test window operation");
        } catch (const NCommon::Exception& exception) {
            rejected = exception.code() == NCommon::make_error_code(NCommon::EError::INVALID_STATE) &&
                       std::string_view{exception.GetMessage()}.find("registered application thread") !=
                               std::string_view::npos;
        }
    }};

    thread.join();

    EXPECT_TRUE(rejected);
}

TEST(WindowRuntime, ValidatesBeforeCreatingGlfwBackend) {
    NWindow::NEngine::SetWindowEngineFactoryForTests(&CreateFakeWindowEngine);

    FakeWindowState state;
    g_fakeState = &state;

    try {
        NWindow::Window window{
                NWindow::WindowConfig{NWindow::EWindowType::GLFW},
        };
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(exception.code(), NCommon::make_error_code(NCommon::EError::INVALID_STATE));
        EXPECT_NE(std::string_view{exception.GetMessage()}.find("WindowRuntime"), std::string_view::npos);

        g_fakeState = nullptr;
        NWindow::NEngine::SetWindowEngineFactoryForTests(nullptr);

        EXPECT_EQ(state.CreatedCount, 0);
        EXPECT_EQ(g_fakeEngine, nullptr);

        return;
    }

    g_fakeState = nullptr;
    NWindow::NEngine::SetWindowEngineFactoryForTests(nullptr);

    FAIL() << "Creating GLFW window without WindowRuntime did not throw";
}

// -----------------------------------------------------------------------------
// Construction and ownership
// -----------------------------------------------------------------------------

TEST_F(WindowTest, CreatesAndOwnsWindowEngine) {
    EXPECT_EQ(m_state.CreatedCount, 0);
    EXPECT_EQ(m_state.DestroyedCount, 0);

    {
        NWindow::Window window{
                NWindow::WindowConfig{NWindow::EWindowType::GLFW},
        };

        EXPECT_EQ(m_state.CreatedCount, 1);
        EXPECT_EQ(m_state.DestroyedCount, 0);
    }

    EXPECT_EQ(m_state.DestroyedCount, 1);
}

TEST_F(WindowTest, PassesConfigToWindowEngineFactory) {
    NWindow::WindowConfig config{
            NWindow::EWindowType::QT,
    };

    config.Title = "Configured window";
    config.Size = {
            .Width = 1600,
            .Height = 900,
    };

    NWindow::Window window{config};

    EXPECT_EQ(m_state.ConfigType, NWindow::EWindowType::QT);

    EXPECT_EQ(m_state.ConfigTitle, "Configured window");

    EXPECT_EQ(m_state.ConfigSize,
              (NWindow::WindowSize{
                      .Width = 1600,
                      .Height = 900,
              }));
}

TEST_F(WindowTest, AttachesEventSink) {
    NWindow::Window window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    ASSERT_NE(g_fakeEngine, nullptr);

    EXPECT_TRUE(g_fakeEngine->HasEventSink());
}

// -----------------------------------------------------------------------------
// Operations and state
// -----------------------------------------------------------------------------

TEST_F(WindowTest, ForwardsTitle) {
    NWindow::Window window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    window.SetTitle("Window title");

    EXPECT_EQ(m_state.Title, "Window title");
}

TEST_F(WindowTest, ForwardsSize) {
    NWindow::Window window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    const NWindow::WindowSize size{
            .Width = 800,
            .Height = 600,
    };

    window.SetSize(size);

    EXPECT_EQ(m_state.Size, size);
}

TEST_F(WindowTest, QueriesWindowSizes) {
    NWindow::Window window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    m_state.Size = {
            .Width = 1280,
            .Height = 720,
    };

    m_state.FramebufferSize = {
            .Width = 2560,
            .Height = 1440,
    };

    EXPECT_EQ(window.GetSize(), m_state.Size);
    EXPECT_EQ(window.GetFramebufferSize(), m_state.FramebufferSize);
}

TEST_F(WindowTest, ForwardsProcessEvents) {
    NWindow::Window window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    window.ProcessEvents();
    window.ProcessEvents();

    EXPECT_EQ(m_state.ProcessEventsCount, 2);
}

// -----------------------------------------------------------------------------
// Resize events
// -----------------------------------------------------------------------------

TEST_F(WindowTest, DeliversResizeEvents) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    ASSERT_NE(g_fakeEngine, nullptr);

    const NWindow::WindowSize windowSize{
            .Width = 900,
            .Height = 700,
    };

    const NWindow::WindowSize framebufferSize{
            .Width = 1800,
            .Height = 1400,
    };

    g_fakeEngine->EmitResize(windowSize);
    g_fakeEngine->EmitFramebufferResize(framebufferSize);

    EXPECT_EQ(window.ResizeCount, 1);
    EXPECT_EQ(window.LastSize, windowSize);

    EXPECT_EQ(window.FramebufferResizeCount, 1);
    EXPECT_EQ(window.LastFramebufferSize, framebufferSize);
}

TEST_F(WindowTest, DoesNotSynthesizeResizeFromSetSize) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    window.SetSize({
            .Width = 800,
            .Height = 600,
    });

    EXPECT_EQ(window.ResizeCount, 0);
    EXPECT_EQ(window.FramebufferResizeCount, 0);
}

TEST(WindowGlfwEventQueue, DispatchesQueuedEventsExplicitly) {
    NWindow::NEngine::WindowEventQueue queue;
    RecordingEventSink eventSink;

    queue.Enqueue({
            .Type = NWindow::NEngine::EWindowEventType::RESIZE,
            .Size =
                    {
                            .Width = 640,
                            .Height = 480,
                    },
    });

    queue.Enqueue({
            .Type = NWindow::NEngine::EWindowEventType::FRAMEBUFFER_RESIZE,
            .Size =
                    {
                            .Width = 1280,
                            .Height = 960,
                    },
    });

    queue.Enqueue({
            .Type = NWindow::NEngine::EWindowEventType::CLOSE,
    });

    EXPECT_TRUE(eventSink.Events.empty());
    EXPECT_FALSE(queue.IsEmpty());

    queue.Dispatch(eventSink);

    EXPECT_TRUE(queue.IsEmpty());
    EXPECT_EQ(eventSink.Events,
              (std::vector<std::string>{
                      "resize",
                      "framebuffer",
                      "close",
              }));
    EXPECT_EQ(eventSink.LastWindowSize,
              (NWindow::WindowSize{
                      .Width = 640,
                      .Height = 480,
              }));
    EXPECT_EQ(eventSink.LastFramebufferSize,
              (NWindow::WindowSize{
                      .Width = 1280,
                      .Height = 960,
              }));
}

// -----------------------------------------------------------------------------
// Close lifecycle
// -----------------------------------------------------------------------------

TEST_F(WindowTest, RequestCloseChangesStateAndNotifies) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    EXPECT_FALSE(window.ShouldClose());

    window.RequestClose();

    EXPECT_TRUE(window.ShouldClose());
    EXPECT_EQ(m_state.RequestCloseCount, 1);
    EXPECT_EQ(window.CloseCount, 1);
}

TEST_F(WindowTest, NativeCloseChangesStateAndNotifies) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    ASSERT_NE(g_fakeEngine, nullptr);

    g_fakeEngine->EmitClose();

    EXPECT_TRUE(window.ShouldClose());
    EXPECT_EQ(window.CloseCount, 1);
}

TEST_F(WindowTest, CloseNotifiesExactlyOnce) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    ASSERT_NE(g_fakeEngine, nullptr);

    window.RequestClose();
    window.RequestClose();

    g_fakeEngine->EmitClose();

    EXPECT_TRUE(window.ShouldClose());
    EXPECT_EQ(m_state.RequestCloseCount, 1);
    EXPECT_EQ(window.CloseCount, 1);
}

// -----------------------------------------------------------------------------
// Factory errors
// -----------------------------------------------------------------------------

TEST_F(WindowTest, RejectsNullEngineFromFactory) {
    NWindow::NEngine::SetWindowEngineFactoryForTests(&CreateNullWindowEngine);

    try {
        NWindow::Window window{
                NWindow::WindowConfig{NWindow::EWindowType::GLFW},
        };
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(exception.code(), NCommon::make_error_code(NCommon::EError::INVALID_STATE));

        EXPECT_EQ(exception.GetMessage(), "Window engine factory returned null");

        return;
    }

    FAIL() << "Null window engine did not throw";
}

TEST(WindowFactory, RejectsUnavailableImplementation) {
    NWindow::NEngine::SetWindowEngineFactoryForTests(nullptr);

    try {
        NWindow::Window window{
                NWindow::WindowConfig{NWindow::EWindowType::QT},
        };
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(exception.code(), NCommon::make_error_code(NCommon::EError::UNSUPPORTED));

        EXPECT_NE(std::string_view{exception.GetMessage()}.find("Qt"), std::string_view::npos);

        return;
    }

    FAIL() << "Creating unavailable window implementation did not throw";
}

} // namespace
