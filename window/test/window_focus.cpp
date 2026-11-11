#include <cstdlib>
#include <format>
#include <glfw.h>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <window/engine/GLFW/window_engine.h>
#include <window/engine/test_process.h>
#include <window/window.h>
#include <window/window_config.h>
#include <window/window_runtime.h>

namespace {

bool IsDisplayUnavailable(std::string_view message) {
    constexpr std::string_view prefix = "Failed to initialize GLFW: ";
    if (!message.starts_with(prefix)) {
        return false;
    }
    message.remove_prefix(prefix.size());
    const std::string platformError = std::format(" ({})", GLFW_PLATFORM_ERROR);
    bool connectionError = message.ends_with(platformError);
#if defined(GLFW_PLATFORM_UNAVAILABLE)
    connectionError = connectionError || message.ends_with(std::format(" ({})", GLFW_PLATFORM_UNAVAILABLE));
#endif
    if (!connectionError) {
        return false;
    }
    const std::string_view diagnostic = message.substr(0, message.rfind(" ("));
    return diagnostic == "X11: The DISPLAY environment variable is missing" ||
           diagnostic.starts_with("X11: Failed to open display ") ||
           diagnostic == "Wayland: Failed to connect to display";
}

GLFWwindow* GetNativeWindow(NWindow::Window& window) {
    auto& engine =
            dynamic_cast<NWindow::NEngine::NGlfw::IGlfwWindowEngine&>(NWindow::NInternal::GetWindowEngine(window));
    return &engine.GetGlfwWindow();
}

void CheckWindow(NWindow::Window& window) {
    GLFWwindow* native = GetNativeWindow(window);
    EXPECT_EQ(glfwGetWindowAttrib(native, GLFW_VISIBLE), GLFW_TRUE);
    EXPECT_EQ(glfwGetWindowAttrib(native, GLFW_FOCUSED), GLFW_FALSE);
    EXPECT_EQ(glfwGetWindowAttrib(native, GLFW_FOCUS_ON_SHOW), GLFW_FALSE);

    glfwHideWindow(native);
    glfwShowWindow(native);
    window.SetSize({.Width = 96, .Height = 80});
    for (int poll = 0; poll != 5; ++poll) {
        glfwWaitEventsTimeout(0.01);
        window.ProcessEvents();
        EXPECT_EQ(glfwGetWindowAttrib(native, GLFW_FOCUSED), GLFW_FALSE);
    }
    EXPECT_EQ(glfwGetWindowAttrib(native, GLFW_VISIBLE), GLFW_TRUE);
    EXPECT_GT(window.GetFramebufferSize().Width, 0);
    EXPECT_GT(window.GetFramebufferSize().Height, 0);
}

} // namespace

TEST(WindowFocus, OnlyKnownDisplayConnectionFailuresAreUnavailable) {
    EXPECT_TRUE(IsDisplayUnavailable(
            "Failed to initialize GLFW: X11: The DISPLAY environment variable is missing (65544)"));
    EXPECT_TRUE(IsDisplayUnavailable("Failed to initialize GLFW: X11: Failed to open display :99 (65544)"));
    EXPECT_TRUE(IsDisplayUnavailable("Failed to initialize GLFW: Wayland: Failed to connect to display (65544)"));
#if defined(GLFW_PLATFORM_UNAVAILABLE)
    EXPECT_TRUE(IsDisplayUnavailable(
            "Failed to initialize GLFW: X11: The DISPLAY environment variable is missing (65550)"));
    EXPECT_TRUE(IsDisplayUnavailable("Failed to initialize GLFW: X11: Failed to open display :99 (65550)"));
#endif

    EXPECT_FALSE(IsDisplayUnavailable("Failed to initialize GLFW"));
    EXPECT_FALSE(
            IsDisplayUnavailable("Failed to initialize GLFW: Cocoa: Failed to create application delegate (65544)"));
    EXPECT_FALSE(IsDisplayUnavailable("Failed to initialize GLFW: Failed to allocate memory (65541)"));
    EXPECT_FALSE(IsDisplayUnavailable("Failed to initialize GLFW: X11: Failed to load Xlib (65544)"));
    EXPECT_FALSE(IsDisplayUnavailable("Failed to initialize GLFW: Failed to detect any supported platform (65550)"));
    EXPECT_FALSE(IsDisplayUnavailable("Failed to initialize GLFW: X11: Failed to open display :99 (65541)"));
    EXPECT_FALSE(IsDisplayUnavailable("Failed to create GLFW window: Wayland: Failed to connect to display (65544)"));
}

TEST(WindowFocus, BootstrapPrecedesRuntimeAndEveryWindowStaysVisibleWithoutFocus) {
    ASSERT_TRUE(NWindow::NEngine::IsNonActivatingTestProcess());
#if defined(__linux__)
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if ((display == nullptr || *display == '\0') && (waylandDisplay == nullptr || *waylandDisplay == '\0')) {
        GTEST_SKIP() << "No X11 DISPLAY or WAYLAND_DISPLAY in this process environment";
    }
#endif
    NWindow::WindowRuntime runtime;
    NWindow::WindowConfig config{NWindow::EWindowType::GLFW};
    config.Title = "GraphicsEngine non-activating window regression";
    config.Size = {.Width = 64, .Height = 64};

    // Only initial display unavailability is a skip; later lifecycle failures are regressions.
    std::unique_ptr<NWindow::Window> first;
    try {
        first = std::make_unique<NWindow::Window>(config);
    } catch (const NCommon::Exception& exception) {
        if (IsDisplayUnavailable(exception.GetMessage())) {
            GTEST_SKIP() << exception.what();
        }
        throw;
    }

    CheckWindow(*first);
    for (int iteration = 0; iteration != 3; ++iteration) {
        // Deliberately contaminate the process-global hints: each backend creation must reset them.
        glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
        NWindow::Window second{config};
        CheckWindow(second);
        second.RequestClose();
    }
    first.reset();
    // Exercise glfwTerminate -> glfwInit as well as multiple windows sharing one runtime.
    NWindow::Window recreated{config};
    CheckWindow(recreated);
    recreated.RequestClose();
}
