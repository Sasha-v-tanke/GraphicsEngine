#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <string>

#include <application/application.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <window/window.h>

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
