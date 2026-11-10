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
    return message == "X11: The DISPLAY environment variable is missing (65544)" ||
           (message.starts_with("X11: Failed to open display ") && message.ends_with(" (65544)")) ||
           message == "Wayland: Failed to connect to display (65544)";
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

    EXPECT_FALSE(IsDisplayUnavailable("Failed to initialize GLFW"));
    EXPECT_FALSE(
            IsDisplayUnavailable("Failed to initialize GLFW: Cocoa: Failed to create application delegate (65544)"));
    EXPECT_FALSE(IsDisplayUnavailable("Failed to initialize GLFW: Failed to allocate memory (65541)"));
    EXPECT_FALSE(IsDisplayUnavailable("Failed to initialize GLFW: X11: Failed to open display :99 (65541)"));
    EXPECT_FALSE(IsDisplayUnavailable("Failed to create GLFW window: Wayland: Failed to connect to display (65544)"));
}

TEST(WindowFocus, BootstrapPrecedesRuntimeAndEveryWindowStaysVisibleWithoutFocus) {
    ASSERT_TRUE(NWindow::NEngine::IsNonActivatingTestProcess());
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
