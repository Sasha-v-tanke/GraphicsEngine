#include <cstdint>

#include <graphics/vulkan/resource_state.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>

namespace {

VkImage ImageA() {
    return reinterpret_cast<VkImage>(0x1000);
}

VkImage ImageB() {
    return reinterpret_cast<VkImage>(0x2000);
}

VkImageSubresourceRange ColorRange() {
    return {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
    };
}

TEST(VulkanResourceStateTracker, ConvertsSemanticImageUsages) {
    const NVulkan::VulkanImageState present = NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::Present, 7);
    EXPECT_EQ(present.StageMask, VK_PIPELINE_STAGE_2_NONE);
    EXPECT_EQ(present.AccessMask, 0U);
    EXPECT_EQ(present.Layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    EXPECT_EQ(present.QueueFamilyIndex, 7U);

    const NVulkan::VulkanImageState color =
            NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::ColorAttachmentWrite, 3);
    EXPECT_EQ(color.StageMask, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    EXPECT_EQ(color.AccessMask, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    EXPECT_EQ(color.Layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(color.QueueFamilyIndex, 3U);
}

TEST(VulkanResourceStateTracker, EmitsBarrierOnlyForRealStateChanges) {
    NVulkan::VulkanResourceStateTracker tracker;
    tracker.ImportImage(ImageA(), NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::Undefined));

    std::optional<VkImageMemoryBarrier2> barrier = tracker.TransitionImage({
            .Image = ImageA(),
            .Usage = NVulkan::EVulkanResourceUsage::Present,
            .SubresourceRange = ColorRange(),
    });

    ASSERT_TRUE(barrier.has_value());
    EXPECT_EQ(barrier->oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(barrier->newLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    EXPECT_EQ(barrier->srcStageMask, VK_PIPELINE_STAGE_2_NONE);
    EXPECT_EQ(barrier->dstStageMask, VK_PIPELINE_STAGE_2_NONE);
    EXPECT_EQ(barrier->srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
    EXPECT_EQ(barrier->dstQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);

    barrier = tracker.TransitionImage({
            .Image = ImageA(),
            .Usage = NVulkan::EVulkanResourceUsage::Present,
            .SubresourceRange = ColorRange(),
    });
    EXPECT_FALSE(barrier.has_value());
}

TEST(VulkanResourceStateTracker, TracksImportedAndUntrackedImages) {
    NVulkan::VulkanResourceStateTracker tracker;

    const std::optional<VkImageMemoryBarrier2> barrier = tracker.TransitionImage({
            .Image = ImageA(),
            .Usage = NVulkan::EVulkanResourceUsage::ColorAttachmentWrite,
            .SubresourceRange = ColorRange(),
            .QueueFamilyIndex = 5,
    });

    ASSERT_TRUE(barrier.has_value());
    EXPECT_EQ(barrier->oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(barrier->newLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(barrier->srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
    EXPECT_EQ(barrier->dstQueueFamilyIndex, 5U);

    const std::optional<NVulkan::VulkanImageState> state = tracker.GetImageState(ImageA());
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->Layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(state->QueueFamilyIndex, 5U);

    tracker.ForgetImage(ImageA());
    EXPECT_FALSE(tracker.GetImageState(ImageA()).has_value());
}

TEST(VulkanResourceStateTracker, EmitsQueueFamilyOwnershipTransfer) {
    NVulkan::VulkanResourceStateTracker tracker;
    tracker.ImportImage(ImageB(), NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::TransferWrite, 2));

    const std::optional<VkImageMemoryBarrier2> barrier = tracker.TransitionImage({
            .Image = ImageB(),
            .Usage = NVulkan::EVulkanResourceUsage::ShaderSampledRead,
            .SubresourceRange = ColorRange(),
            .QueueFamilyIndex = 4,
    });

    ASSERT_TRUE(barrier.has_value());
    EXPECT_EQ(barrier->oldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT_EQ(barrier->newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(barrier->srcQueueFamilyIndex, 2U);
    EXPECT_EQ(barrier->dstQueueFamilyIndex, 4U);
    EXPECT_EQ(barrier->srcAccessMask, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    EXPECT_EQ(barrier->dstAccessMask, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

TEST(VulkanResourceStateTracker, RejectsNullImages) {
    NVulkan::VulkanResourceStateTracker tracker;

    EXPECT_THROW(tracker.ImportImage(VK_NULL_HANDLE, {}), NCommon::Exception);
    EXPECT_THROW((void)tracker.TransitionImage({
                         .Image = VK_NULL_HANDLE,
                         .Usage = NVulkan::EVulkanResourceUsage::Present,
                         .SubresourceRange = ColorRange(),
                 }),
                 NCommon::Exception);
}

} // namespace
