#include <atomic>

#include <graphics/vulkan/test/wsi_result_override.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <samples/sample_5_triangle/scenario.h>
#include <tests/common/test_error.h>

namespace {
TEST(GraphPresentation, IndexedTriangleThroughWorldGraphAndEngineWithResizeAndMultipleSlots) {
    std::atomic<int> errors{0};
    const NGraphics::PresentationConfig config{.RequireValidation = true,
                                               .ValidationErrorHandler = [&](std::string_view message) {
                                                   ++errors;
                                                   std::cerr << message << '\n';
                                               }};
    try {
        for (std::size_t count: {1U, 3U}) {
            const auto result = NTriangle::Run(count, count, 24, true, config);
            EXPECT_GE(result.Submitted, 24U);
            EXPECT_EQ(result.Submitted, result.Completed);
            EXPECT_EQ(result.Completed, result.Recycled);
        }
    } catch (const NCommon::Exception& error) {
        if (error.code() == NCommon::EError::UNSUPPORTED)
            GTEST_SKIP() << error.GetMessage();
        throw;
    }
    EXPECT_EQ(errors.load(), 0);
}
TEST(GraphPresentation, AcquireRetriesDoNotBuildGraphsAndConsumedTargetsExpire) {
    std::atomic<int> errors{0};
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{NWindow::WindowConfig{NWindow::EWindowType::GLFW}};
        NRenderer::GraphPresentation presentation{window,
                                                  {.MaxActiveFrames = 1,
                                                   .RequireValidation = true,
                                                   .ValidationErrorHandler = [&](std::string_view message) {
                                                       ++errors;
                                                       std::cerr << message << '\n';
                                                   }}};
        NVulkan::NTest::SwapchainResultOverride inject;
        int builds = 0;
        std::shared_ptr<const NRenderer::SwapchainGraphTarget> consumed;
        auto build = [&](const auto& target) {
            ++builds;
            consumed = target;
            NRenderer::RenderGraphBuilder builder{{.Generation = 1}};
            const auto color = builder.ImportSwapchain(target, NRenderer::EResourceUsage::UNDEFINED);
            NRenderer::GraphicsPassDescription pass;
            pass.Writes = {{color, NRenderer::EResourceUsage::COLOR_ATTACHMENT_WRITE}};
            pass.ColorAttachments = {{.Resource = color}};
            (void)builder.AddGraphicsPass(pass);
            pass.ColorAttachments[0].Load = NRenderer::EAttachmentLoad::LOAD;
            (void)builder.AddGraphicsPass(pass);
            return builder.Compile();
        };
        NVulkan::NTest::SwapchainResultOverride::NextAcquire = VK_NOT_READY;
        EXPECT_EQ(presentation.Render({.Generation = 1}, build).Status, NGraphics::EFrameSubmissionStatus::NOT_READY);
        NVulkan::NTest::SwapchainResultOverride::NextAcquire = VK_ERROR_OUT_OF_DATE_KHR;
        EXPECT_EQ(presentation.Render({.Generation = 1}, build).Status, NGraphics::EFrameSubmissionStatus::OUT_OF_DATE);
        presentation.SetPresentationSize(0, 0);
        EXPECT_EQ(presentation.Render({.Generation = 1}, build).Status, NGraphics::EFrameSubmissionStatus::SUSPENDED);
        EXPECT_EQ(builds, 0);
        const auto size = window.GetFramebufferSize();
        presentation.SetPresentationSize(size.Width, size.Height);
        NGraphics::FrameSubmissionResult result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        NVulkan::NTest::SwapchainResultOverride::NextPresent = VK_ERROR_SURFACE_LOST_KHR;
        do {
            result = presentation.Render({.Generation = 1}, build);
            ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        } while (!result.Completion.IsValid());
        EXPECT_EQ(result.Status, NGraphics::EFrameSubmissionStatus::FAILED);
        EXPECT_TRUE(result.Failure);
        EXPECT_EQ(builds, 1);
        ASSERT_TRUE(consumed);
        EXPECT_FALSE(consumed->IsValid());
        while (!presentation.IsCompleted(result.Completion)) {
            ASSERT_LT(std::chrono::steady_clock::now(), deadline);
            std::this_thread::yield();
        }
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)presentation.IsCompleted({}); });
        const auto failed = presentation.Render({.ApplicationFrameIndex = 1, .Generation = 1}, [&](const auto&) {
            NRenderer::RenderGraphBuilder stale{{.ApplicationFrameIndex = 1, .Generation = 1}};
            (void)stale.ImportSwapchain(consumed, NRenderer::EResourceUsage::UNDEFINED);
            return stale.Compile();
        });
        EXPECT_EQ(failed.Status, NGraphics::EFrameSubmissionStatus::FAILED);
        EXPECT_FALSE(failed.Completion.IsValid());
        EXPECT_TRUE(failed.Failure);
    } catch (const NCommon::Exception& error) {
        if (error.code() == NCommon::EError::UNSUPPORTED)
            GTEST_SKIP() << error.GetMessage();
        throw;
    }
    EXPECT_EQ(errors.load(), 0);
}
} // namespace
