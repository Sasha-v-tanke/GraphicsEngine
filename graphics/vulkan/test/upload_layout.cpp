#include <cstdint>
#include <limits>

#include <graphics/vulkan/upload_layout.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>

TEST(VulkanUploadLayout, PacksOddImageAndBufferSlicesWithoutOverlap) {
    std::uint64_t end = 0;
    EXPECT_EQ(NVulkan::NInternal::AppendUploadSlice(end, 3), 0U);
    EXPECT_EQ(NVulkan::NInternal::AppendUploadSlice(end, 1), 4U);
    EXPECT_EQ(NVulkan::NInternal::AppendUploadSlice(end, 1024), 8U);
    EXPECT_EQ(end, 1032U);
}

TEST(VulkanUploadLayout, RejectsOverflowBeforeChangingLayout) {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t end = maximum - 2;
    EXPECT_THROW((void)NVulkan::NInternal::AppendUploadSlice(end, 1), NCommon::Exception);
    EXPECT_EQ(end, maximum - 2);
    end = maximum - 3;
    EXPECT_THROW((void)NVulkan::NInternal::AppendUploadSlice(end, 4), NCommon::Exception);
    EXPECT_EQ(end, maximum - 3);
    EXPECT_EQ(NVulkan::NInternal::AppendUploadSlice(end, 3), maximum - 3);
    EXPECT_EQ(end, maximum);
}
