#include <cstddef>
#include <limits>
#include <vector>

#include <gtest/gtest.h>
#include <renderer/recording_plan.h>
#include <tests/common/test_error.h>

namespace {

TEST(RecordingPlan, CoalescesLargeUnevenPassesWithoutChangingCoverageOrOrder) {
    for (const std::size_t workers: {1U, 3U, 8U}) {
        std::vector<NRenderer::GraphRecordingRange> input{{0, 0, 0}};
        for (std::size_t first = 0; first < 10001; first += 64) {
            input.push_back({1, first, std::min<std::size_t>(64, 10001 - first)});
        }
        input.push_back({2, 0, 3});
        const auto output = NRenderer::NInternal::PlanRecordingChunks(input, workers);
        ASSERT_GE(output.size(), 3U);
        EXPECT_LE(output.size(), workers * 2 + 2);
        EXPECT_EQ(output.front().PassIndex, 0U);
        EXPECT_EQ(output.front().DrawCount, 0U);
        std::size_t next = 0;
        for (std::size_t index = 1; index + 1 < output.size(); ++index) {
            EXPECT_EQ(output[index].PassIndex, 1U);
            EXPECT_EQ(output[index].FirstDraw, next);
            EXPECT_GT(output[index].DrawCount, 0U);
            next += output[index].DrawCount;
        }
        EXPECT_EQ(next, 10001U);
        EXPECT_EQ(output.back().PassIndex, 2U);
        EXPECT_EQ(output.back().FirstDraw, 0U);
        EXPECT_EQ(output.back().DrawCount, 3U);
    }
}

TEST(RecordingPlan, EmptyAndExtremeWorkerCountsAreBounded) {
    EXPECT_TRUE(NRenderer::NInternal::PlanRecordingChunks({}, 1).empty());
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [] { static_cast<void>(NRenderer::NInternal::PlanRecordingChunks({}, 0)); });
    const std::vector<NRenderer::GraphRecordingRange> ranges{{0, 0, 64}, {0, 64, 1}};
    const auto chunks = NRenderer::NInternal::PlanRecordingChunks(ranges, std::numeric_limits<std::size_t>::max());
    ASSERT_EQ(chunks.size(), 2U);
    EXPECT_EQ(chunks[1].FirstDraw, 64U);
    EXPECT_EQ(chunks[1].DrawCount, 1U);
}

} // namespace
