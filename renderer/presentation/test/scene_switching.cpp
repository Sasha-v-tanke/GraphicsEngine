#include <atomic>
#include <iostream>

#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <samples/sample_8_scene_switching/scenario.h>

namespace {

TEST(GraphPresentation, SceneSwitchingDuringDelayedIoKeepsSharedAssetsAndActiveSlotsValid) {
    std::atomic<int> errors{0};
    const NGraphics::PresentationConfig validation{.RequireValidation = true,
                                                   .ValidationErrorHandler = [&](std::string_view message) {
                                                       ++errors;
                                                       std::cerr << message << '\n';
                                                   }};
    try {
        for (const std::size_t workers: {1U, 3U}) {
            for (const std::size_t slots: {1U, 2U, 3U}) {
                SCOPED_TRACE(testing::Message{} << "workers=" << workers << " slots=" << slots);
                const auto result = NSceneSwitching::Run(slots, workers, 48, false, validation);
                EXPECT_EQ(result.Submitted, 48U);
                EXPECT_EQ(result.Completed, result.Submitted);
                EXPECT_EQ(result.Recycled, result.Completed);
                EXPECT_EQ(result.Publications, result.Submitted);
                EXPECT_GT(result.LoadingFrames, 0U);
                EXPECT_GT(result.ReadyFrames, 0U);
                EXPECT_GT(result.EmptyFrames, 0U);
            }
        }
    } catch (const NCommon::Exception& error) {
        if (error.code() == NCommon::EError::UNSUPPORTED) {
            GTEST_SKIP() << error.GetMessage();
        }
        throw;
    }
    EXPECT_EQ(errors.load(), 0);
}

} // namespace
