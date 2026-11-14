#include "model_visibility.h"

#include <atomic>
#include <string>
#include <thread>

#include <graphics/vulkan/test/wsi_result_override.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <renderer/presentation/graph_state.h>
#include <samples/sample_5_triangle/scenario.h>
#include <samples/sample_6_textured/scenario.h>
#include <samples/sample_7_model/scenario.h>
#include <tests/common/test_error.h>

namespace {
struct UnloadedTextureScene: NTextured::Scene {
    void Prepare(NRenderer::GraphPresentation& presentation) {
        NTextured::Scene::Prepare(presentation);
        Resources.RequestUnload(Image);
        Resources.CompleteUnload(Image);
        Resources.Forget(Image);
        Resources.RequestUnload(Sampler);
        Resources.CompleteUnload(Sampler);
        Resources.Forget(Sampler);
    }
};

TEST(GraphPresentation, TexturedWorldGraphRetainsResourcesAcrossWorkerAndSlotMatrix) {
    std::atomic<int> errors{0};
    const NGraphics::PresentationConfig config{.RequireValidation = true,
                                               .ValidationErrorHandler = [&](std::string_view message) {
                                                   ++errors;
                                                   std::cerr << message << '\n';
                                               }};
    try {
        for (std::size_t workers: {1U, 3U}) {
            for (std::size_t slots: {1U, 3U}) {
                const auto result = NTriangle::Run<UnloadedTextureScene>(slots, workers, 24, true, config);
                EXPECT_EQ(result.Submitted, 24U);
                EXPECT_EQ(result.Submitted, result.Completed);
                EXPECT_EQ(result.Completed, result.Recycled);
            }
        }
    } catch (const NCommon::Exception& error) {
        if (error.code() == NCommon::EError::UNSUPPORTED) {
            EXPECT_EQ(errors.load(), 0);
            GTEST_SKIP() << error.GetMessage();
        }
        throw;
    }
    EXPECT_EQ(errors.load(), 0);
}

TEST(GraphPresentation, TexturedObjModelThroughPerspectiveCameraResizeAndSlotMatrix) {
    std::atomic<int> errors{0};
    const NGraphics::PresentationConfig config{
            .RequireValidation = true,
            .ValidationErrorHandler =
                    [&](std::string_view message) {
                        ++errors;
                        std::cerr << message << '\n';
                    },
    };
    try {
        for (std::size_t workers: {1U, 3U}) {
            for (std::size_t slots: {1U, 2U, 3U}) {
                const auto result = NTriangle::Run<NModel::Scene>(slots, workers, 48, true, config);
                EXPECT_EQ(result.Submitted, 48U);
                EXPECT_EQ(result.Submitted, result.Completed);
                EXPECT_EQ(result.Completed, result.Recycled);
            }
        }
    } catch (const NCommon::Exception& error) {
        if (error.code() == NCommon::EError::UNSUPPORTED && errors.load() == 0) {
            GTEST_SKIP() << error.GetMessage();
        }
        throw;
    }
    EXPECT_EQ(errors.load(), 0);
}

struct VisibilityScene: NModel::Scene {
    inline static NModelTest::VisibilityProbe* Probe = nullptr;
    void Prepare(NRenderer::GraphPresentation& presentation) {
        Probe->Install();
        NModel::Scene::Prepare(presentation);
    }
};

TEST(GraphPresentation, TexturedObjCullsFarFaceAndRasterizesCameraFacingSurface) {
    for (bool flipped: {false, true}) {
        SCOPED_TRACE(flipped ? "Negative control: flipped native winding" : "Unchanged sample winding");
        NModelTest::VisibilityProbe probe;
        probe.FlipFrontFace = flipped;
        VisibilityScene::Probe = &probe;
        std::atomic<int> errors{0};
        try {
            const auto result = NTriangle::Run<VisibilityScene>(
                    1,
                    1,
                    1,
                    false,
                    {.RequireValidation = true, .ValidationErrorHandler = [&](std::string_view message) {
                         ++errors;
                         std::cerr << message << '\n';
                     }});
            EXPECT_EQ(result.Completed, 1U);
        } catch (const NCommon::Exception& error) {
            if (error.code() == NCommon::EError::UNSUPPORTED && errors.load() == 0) {
                GTEST_SKIP() << error.GetMessage();
            }
            throw;
        }
        EXPECT_EQ(errors.load(), 0);
        EXPECT_FALSE(probe.Failed);
        ASSERT_TRUE(probe.Observed);
        RecordProperty(flipped ? "flipped_front" : "sample_front", std::to_string(probe.Samples[0]));
        RecordProperty(flipped ? "flipped_back" : "sample_back", std::to_string(probe.Samples[1]));
        EXPECT_GT(probe.Samples[flipped ? 1 : 0], 0U) << "Expected face must rasterize actual GPU samples";
        EXPECT_EQ(probe.Samples[flipped ? 0 : 1], 0U) << "Opposite face must be culled";
    }
}

TEST(GraphPresentation, RejectsMissingSampledResourcesBeforeRecording) {
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{NWindow::WindowConfig{NWindow::EWindowType::GLFW}};
        NRenderer::GraphPresentation presentation{window, {.RequireValidation = true}};
        NTextured::Scene scene;
        const auto pipeline = scene.Pipeline(presentation.GetColorFormat());
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
            presentation.PrepareMaterial(std::move(*scene.Resources.TryAcquire(scene.Material)), pipeline);
        });
        presentation.PrepareImage(scene.Resources, scene.Image);
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
            presentation.PrepareMaterial(std::move(*scene.Resources.TryAcquire(scene.Material)), pipeline);
        });
        presentation.PrepareSampler(scene.Resources, scene.Sampler);
        presentation.PrepareMaterial(std::move(*scene.Resources.TryAcquire(scene.Material)), pipeline);
        // A duplicate prepare preserves the frozen version and its compatible pipeline.
        presentation.PrepareMaterial(std::move(*scene.Resources.TryAcquire(scene.Material)), pipeline);
        auto incompatible = pipeline;
        incompatible.MaterialBindings = {{1,
                                          NGraphics::EMaterialBindingType::SampledImage,
                                          NGraphics::ShaderVisibility(NGraphics::EShaderVisibility::Fragment)}};
        NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
            presentation.PrepareMaterial(std::move(*scene.Resources.TryAcquire(scene.Material)), incompatible);
        });
    } catch (const NCommon::Exception& error) {
        if (error.code() == NCommon::EError::UNSUPPORTED)
            GTEST_SKIP() << error.GetMessage();
        throw;
    }
}

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

TEST(GraphPresentation, CompiledSemanticsLowerThroughSharedStateTracker) {
    using NRenderer::EResourceUsage;
    using NRenderer::NInternal::VulkanUsage;
    EXPECT_EQ(NVulkan::MakeVulkanImageState(VulkanUsage(EResourceUsage::COLOR_ATTACHMENT_WRITE, false)).AccessMask,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    EXPECT_EQ(NVulkan::MakeVulkanImageState(VulkanUsage(EResourceUsage::COLOR_ATTACHMENT_WRITE, true)).AccessMask,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    EXPECT_EQ(NVulkan::MakeVulkanImageState(VulkanUsage(EResourceUsage::DEPTH_STENCIL_WRITE, true)).Layout,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(NVulkan::MakeVulkanBufferState(VulkanUsage(EResourceUsage::VERTEX_READ)).StageMask,
              VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT);
    EXPECT_EQ(NVulkan::MakeVulkanBufferState(VulkanUsage(EResourceUsage::INDEX_READ)).AccessMask,
              VK_ACCESS_2_INDEX_READ_BIT);
    EXPECT_EQ(NVulkan::MakeVulkanBufferState(VulkanUsage(EResourceUsage::UNIFORM_READ)).AccessMask,
              VK_ACCESS_2_UNIFORM_READ_BIT);
    EXPECT_EQ(NVulkan::MakeVulkanImageState(VulkanUsage(EResourceUsage::TRANSFER_SOURCE)).Layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    EXPECT_EQ(NVulkan::MakeVulkanImageState(VulkanUsage(EResourceUsage::TRANSFER_DESTINATION)).Layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT_EQ(NVulkan::MakeVulkanImageState(VulkanUsage(EResourceUsage::SAMPLED_READ)).AccessMask,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    EXPECT_EQ(NVulkan::MakeVulkanImageState(VulkanUsage(EResourceUsage::PRESENT)).Layout,
              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

TEST(GraphPresentation, TransientMultipassPoolingResizeAndFrameSlotsValidationClean) {
    std::atomic<int> errors{0};
    try {
        NWindow::WindowRuntime runtime;
        NWindow::Window window{NWindow::WindowConfig{NWindow::EWindowType::GLFW}};
        for (std::size_t slots: {1U, 3U}) {
            NRenderer::GraphPresentation presentation{window,
                                                      {.MaxActiveFrames = slots,
                                                       .RequireValidation = true,
                                                       .ValidationErrorHandler = [&](std::string_view message) {
                                                           ++errors;
                                                           std::cerr << message << '\n';
                                                       }}};
            std::vector<NGraphics::CompletionPoint> completions;
            for (std::size_t index = 0; index < 24; ++index) {
                if (index == 12) {
                    presentation.SetPresentationSize(128, 96);
                }
                const NRenderer::RenderFrameIdentity frame{.ApplicationFrameIndex = index,
                                                           .FrameSlotIndex = index % slots,
                                                           .Generation = index / slots + 1};
                bool submitted = false;
                for (std::size_t retry = 0; retry < 10000 && !submitted; ++retry) {
                    const auto result = presentation.Render(frame, [&](const auto& target) {
                        NRenderer::RenderGraphBuilder builder{frame};
                        const auto output = builder.ImportSwapchain(target, NRenderer::EResourceUsage::UNDEFINED);
                        const NRenderer::GraphImageDescription description{
                                {32U + static_cast<std::uint32_t>(index % 2), 32, 1},
                                NGraphics::EPixelFormat::RGBA8_UNORM,
                                NGraphics::ESampleCount::X1,
                                NGraphics::EImageUsage::ColorAttachment | NGraphics::EImageUsage::Sampled};
                        const auto a = builder.CreateTransientImage(description);
                        const auto b = builder.CreateTransientImage(description);
                        const auto dead = builder.CreateTransientImage(description);
                        const auto color = [&](NRenderer::GraphResourceId image) {
                            NRenderer::GraphicsPassDescription pass;
                            pass.Writes = {{image, NRenderer::EResourceUsage::COLOR_ATTACHMENT_WRITE}};
                            pass.ColorAttachments = {{.Resource = image}};
                            return pass;
                        };
                        (void)builder.AddGraphicsPass(color(dead));
                        const auto first = builder.AddGraphicsPass(color(a));
                        const auto second = builder.AddGraphicsPass(color(b));
                        const auto final = builder.AddGraphicsPass(color(output));
                        builder.AddDependency(first, second);
                        builder.AddDependency(second, final);
                        const auto graph = builder.Compile();
                        EXPECT_EQ(graph.GetPasses().size(), 3);
                        EXPECT_EQ(graph.GetLifetimes()[0].Allocation, graph.GetLifetimes()[1].Allocation);
                        return graph;
                    });
                    if (result.Failure) {
                        std::rethrow_exception(result.Failure);
                    }
                    if (result.Completion.IsValid()) {
                        completions.push_back(result.Completion);
                        submitted = true;
                    } else {
                        std::this_thread::yield();
                    }
                }
                ASSERT_TRUE(submitted);
            }
            for (const auto completion: completions) {
                std::size_t retry = 0;
                while (!presentation.IsCompleted(completion) && ++retry < 1000000) {
                    std::this_thread::yield();
                }
                EXPECT_TRUE(presentation.IsCompleted(completion));
            }
        }
    } catch (const NCommon::Exception& error) {
        if (error.code() == NCommon::EError::UNSUPPORTED && errors.load() == 0) {
            GTEST_SKIP() << error.GetMessage();
        }
        throw;
    }
    EXPECT_EQ(errors.load(), 0);
}

} // namespace
