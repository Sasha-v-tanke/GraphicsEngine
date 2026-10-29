#include <atomic>
#include <chrono>
#include <exception>
#include <glfw.h>
#include <iostream>
#include <string>
#include <thread>

#include <application/application.h>
#include <application/runtime/frame_loop.h>
#include <graphics/graphics.h>
#include <graphics/vulkan/test/wsi_result_override.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <window/engine/GLFW/window_engine.h>
#include <window/internal/window_engine_access.h>
#include <window/window.h>
#include <window/window_runtime.h>

namespace {
NApplication::ApplicationConfig Config(std::size_t count, std::atomic<int>& validationErrors) {
    NWindow::WindowConfig window{NWindow::EWindowType::GLFW};
    window.Title = "First GPU Frame integration";
    window.Size = {.Width = 160, .Height = 120};
    return {
            .Window = window,
            .MaxActiveFrames = count,
            .WorkerCount = 2,
            .Presentation =
                    NGraphics::PresentationConfig{
                            .RequireValidation = true,
                            .ValidationErrorHandler =
                                    [&validationErrors](std::string_view message) {
                                        ++validationErrors;
                                        std::cerr << message << '\n';
                                    },
                    },
    };
}

class ClearApplication final: public NApplication::Application {
public:
    ClearApplication(std::size_t count, std::atomic<int>& validationErrors)
        : Application(Config(count, validationErrors)) {
    }

private:
    void OnUpdate() override {
        const auto stats = GetFrameDiagnostics();
        if (!m_resized && stats.Submitted >= 5) {
            GetWindow().SetSize({.Width = 240, .Height = 180});
            m_resized = true;
        }
        if (stats.Submitted >= 24) {
            RequestShutdown();
        }
        if (std::chrono::steady_clock::now() - m_started > std::chrono::seconds{15}) {
            throw std::runtime_error{"First GPU Frame integration timed out"};
        }
    }
    bool m_resized = false;
    std::chrono::steady_clock::time_point m_started = std::chrono::steady_clock::now();
};

bool UnsupportedEnvironment(const NCommon::Exception& error) {
    const auto& message = error.GetMessage();
    return message.find("VK_ERROR_INCOMPATIBLE_DRIVER") != std::string::npos ||
           message.find("Failed to initialize GLFW") != std::string::npos ||
           message.find("Failed to create GLFW window") != std::string::npos ||
           message.find("Failed to get GLFW Vulkan instance extensions") != std::string::npos ||
           message.find("validation layer") != std::string::npos ||
           message.find("No suitable Vulkan physical device") != std::string::npos;
}

void RunClearApplication(std::size_t count) {
    std::atomic<int> validationErrors{0};
    // Only initialization unavailability may skip. Runtime/validation failures always fail.
    std::unique_ptr<ClearApplication> application;
    try {
        application = std::make_unique<ClearApplication>(count, validationErrors);
    } catch (const NCommon::Exception& error) {
        if (UnsupportedEnvironment(error) && validationErrors == 0) {
            GTEST_SKIP() << error.GetMessage();
        }
        throw;
    }
    application->Run();
    const auto diagnostics = application->GetFrameDiagnostics();
    EXPECT_GE(diagnostics.Submitted, 24);
    EXPECT_EQ(diagnostics.Submitted, diagnostics.Completed);
    EXPECT_EQ(diagnostics.Completed, diagnostics.Recycled);
    application.reset();
    EXPECT_EQ(validationErrors.load(), 0);
}

TEST(ApplicationPresentation, SingleFrameRingResizeAndShutdown) {
    RunClearApplication(1);
}
TEST(ApplicationPresentation, MultipleFrameRingResizeAndShutdown) {
    RunClearApplication(3);
}
} // namespace

namespace {
class LifecycleCallbacks final: public NApplication::NRuntime::IFrameLoopCallbacks {
public:
    void OnUpdate() override {
        ++Updates;
    }
    void OnDraw() override {
        ++Draws;
    }
    std::uint64_t Updates = 0;
    std::uint64_t Draws = 0;
};

void RunPresentationLifecycle(std::size_t count) {
    std::atomic<int> validationErrors{0};
    const auto config = Config(count, validationErrors);
    std::unique_ptr<NWindow::WindowRuntime> windowRuntime;
    std::unique_ptr<NWindow::Window> window;
    std::unique_ptr<NGraphics::Graphics> graphics;
    try {
        windowRuntime = std::make_unique<NWindow::WindowRuntime>();
        window = std::make_unique<NWindow::Window>(config.Window);
        auto presentation = *config.Presentation;
        presentation.MaxActiveFrames = count;
        graphics = NGraphics::CreatePresentationGraphics(*window, presentation);
    } catch (const NCommon::Exception& error) {
        if (UnsupportedEnvironment(error) && validationErrors == 0) {
            GTEST_SKIP() << error.GetMessage();
        }
        throw;
    }
    {
        // Exercise the same Engine/checkpoint/presentation pump as Application, controlling only platform intake.
        NApplication::NRuntime::FrameLoop loop{config, std::move(graphics)};
        const NVulkan::NTest::SwapchainResultOverride results;
        LifecycleCallbacks callbacks;
        auto& native =
                dynamic_cast<NWindow::NEngine::NGlfw::IGlfwWindowEngine&>(NWindow::NInternal::GetWindowEngine(*window))
                        .GetGlfwWindow();
        auto step = [&](bool suspended) {
            window->ProcessEvents();
            const auto size = suspended ? NWindow::WindowSize{} : window->GetFramebufferSize();
            loop.PumpPresentation(size.Width, size.Height);
            loop.Step(callbacks);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        };
        auto advance = [&](std::uint64_t target) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (loop.GetDiagnostics().Submitted < target && std::chrono::steady_clock::now() < deadline) {
                step(false);
            }
            return loop.GetDiagnostics().Submitted == target;
        };
        loop.Start();
        ASSERT_TRUE(advance(5));
        for (int cycle = 0; cycle < 2; ++cycle) {
            const auto submitted = loop.GetDiagnostics().Submitted;
            glfwIconifyWindow(&native);
            // Some WSI implementations retain a nonzero framebuffer when iconified. Supply zero extent explicitly.
            for (int index = 0; index < 100; ++index) {
                step(true);
            }
            const auto updates = callbacks.Updates;
            const auto draws = callbacks.Draws;
            for (int index = 0; index < 20; ++index) {
                step(true);
            }
            EXPECT_EQ(loop.GetDiagnostics().Submitted, submitted);
            EXPECT_EQ(callbacks.Updates, updates);
            EXPECT_EQ(callbacks.Draws, draws);
            glfwRestoreWindow(&native);
            window->SetSize({.Width = 200 + cycle * 16, .Height = 160 + cycle * 16});
            ASSERT_TRUE(advance(submitted + 4));
        }
        // Inject statuses at the Vulkan dispatch boundary; successful acquire/present still reach the real driver.
        for (const auto status: {VK_NOT_READY, VK_ERROR_OUT_OF_DATE_KHR, VK_SUBOPTIMAL_KHR}) {
            const auto submitted = loop.GetDiagnostics().Submitted;
            NVulkan::NTest::SwapchainResultOverride::NextAcquire = status;
            ASSERT_TRUE(advance(submitted + 2));
            EXPECT_FALSE(NVulkan::NTest::SwapchainResultOverride::NextAcquire.has_value());
        }
        for (const auto status: {VK_ERROR_OUT_OF_DATE_KHR, VK_SUBOPTIMAL_KHR}) {
            const auto submitted = loop.GetDiagnostics().Submitted;
            NVulkan::NTest::SwapchainResultOverride::NextPresent = status;
            ASSERT_TRUE(advance(submitted + 2));
            EXPECT_FALSE(NVulkan::NTest::SwapchainResultOverride::NextPresent.has_value());
        }
        loop.Stop();
        loop.CheckError();
        const auto diagnostics = loop.GetDiagnostics();
        EXPECT_EQ(diagnostics.Submitted, diagnostics.Completed);
        EXPECT_EQ(diagnostics.Completed, diagnostics.Recycled);
        loop.Start();
        ASSERT_TRUE(advance(5));
        loop.Stop();
        loop.CheckError();
        EXPECT_EQ(loop.GetDiagnostics().Submitted, loop.GetDiagnostics().Recycled);
    }
    window.reset();
    windowRuntime.reset();
    EXPECT_EQ(validationErrors.load(), 0);
}

TEST(ApplicationPresentation, SingleFrameRingZeroExtentRestoreAndControlledWsiStatuses) {
    RunPresentationLifecycle(1);
}
TEST(ApplicationPresentation, MultipleFrameRingZeroExtentRestoreAndControlledWsiStatuses) {
    RunPresentationLifecycle(3);
}
} // namespace
