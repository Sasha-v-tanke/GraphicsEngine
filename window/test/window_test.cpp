#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>
#include <window/internal/engine.h>
#include <window/internal/event_sink.h>
#include <window/internal/factory.h>
#include <window/window.h>
#include <window/window_config.h>
#include <window/window_size.h>
#include <window/window_type.h>

namespace {

struct FakeWindowState {
    std::string Title;

    NWindow::WindowSize Size{
            .Width = 1280,
            .Height = 720,
    };

    NWindow::WindowSize FramebufferSize{
            .Width = 1280,
            .Height = 720,
    };

    bool ShouldClose = false;

    int CreatedCount = 0;
    int DestroyedCount = 0;
    int ProcessEventsCount = 0;
    int RequestCloseCount = 0;
};

thread_local FakeWindowState* g_fakeState = nullptr;

class FakeWindowEngine final: public NWindow::NInternal::IWindowEngine {
public:
    FakeWindowEngine(NWindow::NInternal::IWindowEventSink& eventSink, FakeWindowState& state)
        : m_eventSink(eventSink)
        , m_state(state) {
        ++m_state.CreatedCount;
    }

    ~FakeWindowEngine() override {
        ++m_state.DestroyedCount;
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
        m_state.Size = size;

        m_eventSink.HandleResize(size);
    }

    void EmitFramebufferResize(NWindow::WindowSize size) {
        m_state.FramebufferSize = size;

        m_eventSink.HandleFramebufferResize(size);
    }

    void EmitClose() {
        m_state.ShouldClose = true;

        m_eventSink.HandleClose();
    }

private:
    NWindow::NInternal::IWindowEventSink& m_eventSink;

    FakeWindowState& m_state;
};

thread_local FakeWindowEngine* g_fakeEngine = nullptr;

std::unique_ptr<NWindow::NInternal::IWindowEngine>
CreateFakeWindowEngine(const NWindow::WindowConfig&, NWindow::NInternal::IWindowEventSink& eventSink) {
    std::unique_ptr<FakeWindowEngine> engine = std::make_unique<FakeWindowEngine>(eventSink, *g_fakeState);

    g_fakeEngine = engine.get();

    return engine;
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
        g_fakeState = &m_state;
        g_fakeEngine = nullptr;

        NWindow::NInternal::SetWindowEngineFactoryForTests(&CreateFakeWindowEngine);
    }

    void TearDown() override {
        NWindow::NInternal::SetWindowEngineFactoryForTests(nullptr);

        g_fakeEngine = nullptr;
        g_fakeState = nullptr;
    }

    FakeWindowState m_state;
};

static_assert(!std::is_copy_constructible_v<NWindow::Window>);
static_assert(!std::is_copy_assignable_v<NWindow::Window>);
static_assert(!std::is_move_constructible_v<NWindow::Window>);
static_assert(!std::is_move_assignable_v<NWindow::Window>);

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

    window.SetSize({
            .Width = 800,
            .Height = 600,
    });

    EXPECT_EQ(m_state.Size,
              (NWindow::WindowSize{
                      .Width = 800,
                      .Height = 600,
              }));
}

TEST_F(WindowTest, QueriesLogicalSize) {
    m_state.Size = {
            .Width = 1920,
            .Height = 1080,
    };

    NWindow::Window window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    EXPECT_EQ(window.GetSize(), m_state.Size);
}

TEST_F(WindowTest, QueriesFramebufferSizeIndependently) {
    m_state.Size = {
            .Width = 1280,
            .Height = 720,
    };

    m_state.FramebufferSize = {
            .Width = 2560,
            .Height = 1440,
    };

    NWindow::Window window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    EXPECT_EQ(window.GetSize(), m_state.Size);

    EXPECT_EQ(window.GetFramebufferSize(), m_state.FramebufferSize);
}

TEST_F(WindowTest, ForwardsProcessEvents) {
    NWindow::Window window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    EXPECT_EQ(m_state.ProcessEventsCount, 0);

    window.ProcessEvents();
    window.ProcessEvents();

    EXPECT_EQ(m_state.ProcessEventsCount, 2);
}

TEST_F(WindowTest, RequestCloseChangesCloseState) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    EXPECT_FALSE(window.ShouldClose());
    EXPECT_EQ(window.CloseCount, 0);

    window.RequestClose();

    EXPECT_TRUE(window.ShouldClose());
    EXPECT_EQ(m_state.RequestCloseCount, 1);
    EXPECT_EQ(window.CloseCount, 1);
}

TEST_F(WindowTest, DeliversResize) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    ASSERT_NE(g_fakeEngine, nullptr);

    const NWindow::WindowSize size{
            .Width = 900,
            .Height = 700,
    };

    g_fakeEngine->EmitResize(size);

    EXPECT_EQ(window.ResizeCount, 1);
    EXPECT_EQ(window.LastSize, size);
}

TEST_F(WindowTest, DeliversFramebufferResizeSeparately) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    ASSERT_NE(g_fakeEngine, nullptr);

    const NWindow::WindowSize size{
            .Width = 1800,
            .Height = 1400,
    };

    g_fakeEngine->EmitFramebufferResize(size);

    EXPECT_EQ(window.ResizeCount, 0);

    EXPECT_EQ(window.FramebufferResizeCount, 1);
    EXPECT_EQ(window.LastFramebufferSize, size);
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

TEST_F(WindowTest, DeliversNativeClose) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    ASSERT_NE(g_fakeEngine, nullptr);

    g_fakeEngine->EmitClose();

    EXPECT_TRUE(window.ShouldClose());
    EXPECT_EQ(window.CloseCount, 1);
}

TEST_F(WindowTest, ProgrammaticCloseNotifiesExactlyOnce) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    window.RequestClose();
    window.RequestClose();

    EXPECT_TRUE(window.ShouldClose());

    EXPECT_EQ(window.CloseCount, 1);

    EXPECT_EQ(m_state.RequestCloseCount, 1);
}

TEST_F(WindowTest, NativeCloseNotifiesExactlyOnce) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    ASSERT_NE(g_fakeEngine, nullptr);

    g_fakeEngine->EmitClose();
    g_fakeEngine->EmitClose();

    EXPECT_EQ(window.CloseCount, 1);
}

TEST_F(WindowTest, NativeThenProgrammaticCloseNotifiesExactlyOnce) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    ASSERT_NE(g_fakeEngine, nullptr);

    g_fakeEngine->EmitClose();

    window.RequestClose();

    EXPECT_EQ(window.CloseCount, 1);

    EXPECT_EQ(m_state.RequestCloseCount, 0);
}

TEST_F(WindowTest, ProgrammaticThenNativeCloseNotifiesExactlyOnce) {
    TestWindow window{
            NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    ASSERT_NE(g_fakeEngine, nullptr);

    window.RequestClose();

    g_fakeEngine->EmitClose();

    EXPECT_EQ(window.CloseCount, 1);
}

TEST(WindowFactory, RejectsUnavailableGlfwImplementation) {
    NWindow::NInternal::SetWindowEngineFactoryForTests(nullptr);

    try {
        NWindow::Window window{
                NWindow::WindowConfig{NWindow::EWindowType::GLFW},
        };
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(exception.code(), NCommon::make_error_code(NCommon::EError::UNSUPPORTED));

        EXPECT_NE(std::string_view{exception.GetMessage()}.find("GLFW"), std::string_view::npos);

        return;
    }

    FAIL() << "Creating unavailable GLFW window did not throw";
}

TEST(WindowFactory, RejectsUnavailableQtImplementation) {
    NWindow::NInternal::SetWindowEngineFactoryForTests(nullptr);

    try {
        NWindow::Window window{
                NWindow::WindowConfig{NWindow::EWindowType::QT},
        };
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(exception.code(), NCommon::make_error_code(NCommon::EError::UNSUPPORTED));

        EXPECT_NE(std::string_view{exception.GetMessage()}.find("Qt"), std::string_view::npos);

        return;
    }

    FAIL() << "Creating unavailable Qt window did not throw";
}

TEST(WindowFactory, RejectsUnavailableSdlImplementation) {
    NWindow::NInternal::SetWindowEngineFactoryForTests(nullptr);

    try {
        NWindow::Window window{
                NWindow::WindowConfig{NWindow::EWindowType::SDL},
        };
    } catch (const NCommon::Exception& exception) {
        EXPECT_EQ(exception.code(), NCommon::make_error_code(NCommon::EError::UNSUPPORTED));

        EXPECT_NE(std::string_view{exception.GetMessage()}.find("SDL"), std::string_view::npos);

        return;
    }

    FAIL() << "Creating unavailable SDL window did not throw";
}

} // namespace
