#include <glfw.h>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <window/engine/GLFW/window_engine.h>
#include <window/engine/test_process.h>
#include <window/window.h>
#include <window/window_config.h>
#include <window/window_runtime.h>

namespace {

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
        if (exception.GetMessage().find("Failed to initialize GLFW") != std::string::npos) {
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
