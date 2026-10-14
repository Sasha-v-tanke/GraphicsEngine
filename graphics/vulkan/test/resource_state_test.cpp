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

VkBuffer BufferA() {
    return reinterpret_cast<VkBuffer>(0x3000);
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

VkImageSubresourceRange ColorMipRange(std::uint32_t mipLevel) {
    return {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = mipLevel,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
    };
}

VkImageSubresourceRange ColorMipChainRange() {
    return {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 2,
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
    tracker.ImportImage(ImageA(),
                        ColorRange(),
                        NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::Undefined));

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
    });

    ASSERT_TRUE(barrier.has_value());
    EXPECT_EQ(barrier->oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(barrier->newLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(barrier->srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
    EXPECT_EQ(barrier->dstQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);

    const std::optional<NVulkan::VulkanImageState> state = tracker.GetImageState(ImageA(), ColorRange());
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->Layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(state->QueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);

    tracker.ForgetImage(ImageA());
    EXPECT_FALSE(tracker.GetImageState(ImageA(), ColorRange()).has_value());
}

TEST(VulkanResourceStateTracker, RejectsUntrackedExplicitQueueFamilyTransition) {
    NVulkan::VulkanResourceStateTracker tracker;

    EXPECT_THROW((void)tracker.TransitionImage({
                         .Image = ImageA(),
                         .Usage = NVulkan::EVulkanResourceUsage::ColorAttachmentWrite,
                         .SubresourceRange = ColorRange(),
                         .QueueFamilyIndex = 5,
                 }),
                 NCommon::Exception);
}

TEST(VulkanResourceStateTracker, EmitsQueueFamilyOwnershipTransfer) {
    NVulkan::VulkanResourceStateTracker tracker;
    tracker.ImportImage(ImageB(),
                        ColorRange(),
                        NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::TransferWrite, 2));

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

TEST(VulkanResourceStateTracker, TracksStatePerSubresourceRange) {
    NVulkan::VulkanResourceStateTracker tracker;
    tracker.ImportImage(ImageA(),
                        ColorMipRange(0),
                        NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::Present));
    tracker.ImportImage(ImageA(),
                        ColorMipRange(1),
                        NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::TransferWrite));

    std::optional<VkImageMemoryBarrier2> barrier = tracker.TransitionImage({
            .Image = ImageA(),
            .Usage = NVulkan::EVulkanResourceUsage::Present,
            .SubresourceRange = ColorMipRange(0),
    });
    EXPECT_FALSE(barrier.has_value());

    barrier = tracker.TransitionImage({
            .Image = ImageA(),
            .Usage = NVulkan::EVulkanResourceUsage::Present,
            .SubresourceRange = ColorMipRange(1),
    });
    ASSERT_TRUE(barrier.has_value());
    EXPECT_EQ(barrier->oldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT_EQ(barrier->newLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    EXPECT_EQ(barrier->subresourceRange.baseMipLevel, 1U);
}

TEST(VulkanResourceStateTracker, ExpandsImportedImageRangesToSubresources) {
    NVulkan::VulkanResourceStateTracker tracker;
    tracker.ImportImage(ImageA(),
                        ColorMipChainRange(),
                        NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::Undefined));

    const std::optional<VkImageMemoryBarrier2> barrier = tracker.TransitionImage({
            .Image = ImageA(),
            .Usage = NVulkan::EVulkanResourceUsage::TransferWrite,
            .SubresourceRange = ColorMipRange(1),
    });

    ASSERT_TRUE(barrier.has_value());
    EXPECT_EQ(barrier->oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(barrier->newLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT_EQ(barrier->subresourceRange.baseMipLevel, 1U);

    const std::optional<NVulkan::VulkanImageState> wholeState = tracker.GetImageState(ImageA(), ColorMipChainRange());
    EXPECT_FALSE(wholeState.has_value());
}

TEST(VulkanResourceStateTracker, EmitsBarrierForRepeatedWriteUsage) {
    NVulkan::VulkanResourceStateTracker tracker;
    tracker.ImportImage(ImageA(),
                        ColorRange(),
                        NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::ColorAttachmentWrite));

    const std::optional<VkImageMemoryBarrier2> barrier = tracker.TransitionImage({
            .Image = ImageA(),
            .Usage = NVulkan::EVulkanResourceUsage::ColorAttachmentWrite,
            .SubresourceRange = ColorRange(),
    });

    ASSERT_TRUE(barrier.has_value());
    EXPECT_EQ(barrier->oldLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(barrier->newLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(barrier->srcAccessMask, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    EXPECT_EQ(barrier->dstAccessMask, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

TEST(VulkanResourceStateTracker, RejectsNullImages) {
    NVulkan::VulkanResourceStateTracker tracker;

    EXPECT_THROW(tracker.ImportImage(VK_NULL_HANDLE, ColorRange(), {}), NCommon::Exception);
    EXPECT_THROW((void)tracker.TransitionImage({
                         .Image = VK_NULL_HANDLE,
                         .Usage = NVulkan::EVulkanResourceUsage::Present,
                         .SubresourceRange = ColorRange(),
                 }),
                 NCommon::Exception);
}

TEST(VulkanResourceStateTracker, TracksBufferRanges) {
    NVulkan::VulkanResourceStateTracker tracker;
    tracker.ImportBuffer(BufferA(),
                         64,
                         128,
                         NVulkan::MakeVulkanBufferState(NVulkan::EVulkanResourceUsage::TransferWrite));

    const std::optional<VkBufferMemoryBarrier2> barrier = tracker.TransitionBuffer({
            .Buffer = BufferA(),
            .Offset = 64,
            .Size = 128,
            .Usage = NVulkan::EVulkanResourceUsage::ShaderSampledRead,
    });

    ASSERT_TRUE(barrier.has_value());
    EXPECT_EQ(barrier->buffer, BufferA());
    EXPECT_EQ(barrier->offset, 64U);
    EXPECT_EQ(barrier->size, 128U);
    EXPECT_EQ(barrier->srcAccessMask, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    EXPECT_EQ(barrier->dstAccessMask, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    const std::optional<NVulkan::VulkanBufferState> state = tracker.GetBufferState(BufferA(), 64, 128);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->AccessMask, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    tracker.ForgetBuffer(BufferA());
    EXPECT_FALSE(tracker.GetBufferState(BufferA(), 64, 128).has_value());
}

TEST(VulkanResourceStateTracker, SplitsBufferRangesForSubrangeTransitions) {
    NVulkan::VulkanResourceStateTracker tracker;
    tracker.ImportBuffer(BufferA(),
                         0,
                         256,
                         NVulkan::MakeVulkanBufferState(NVulkan::EVulkanResourceUsage::TransferWrite));

    const std::optional<VkBufferMemoryBarrier2> barrier = tracker.TransitionBuffer({
            .Buffer = BufferA(),
            .Offset = 64,
            .Size = 128,
            .Usage = NVulkan::EVulkanResourceUsage::ShaderSampledRead,
    });

    ASSERT_TRUE(barrier.has_value());
    EXPECT_EQ(barrier->offset, 64U);
    EXPECT_EQ(barrier->size, 128U);
    EXPECT_EQ(barrier->srcAccessMask, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    EXPECT_EQ(barrier->dstAccessMask, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    const std::optional<NVulkan::VulkanBufferState> leading = tracker.GetBufferState(BufferA(), 0, 64);
    ASSERT_TRUE(leading.has_value());
    EXPECT_EQ(leading->AccessMask, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const std::optional<NVulkan::VulkanBufferState> middle = tracker.GetBufferState(BufferA(), 64, 128);
    ASSERT_TRUE(middle.has_value());
    EXPECT_EQ(middle->AccessMask, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    const std::optional<NVulkan::VulkanBufferState> whole = tracker.GetBufferState(BufferA(), 0, 256);
    EXPECT_FALSE(whole.has_value());
}

TEST(VulkanResourceStateTracker, EmitsBufferBarrierForRepeatedWriteUsage) {
    NVulkan::VulkanResourceStateTracker tracker;
    tracker.ImportBuffer(BufferA(),
                         0,
                         VK_WHOLE_SIZE,
                         NVulkan::MakeVulkanBufferState(NVulkan::EVulkanResourceUsage::TransferWrite));

    const std::optional<VkBufferMemoryBarrier2> barrier = tracker.TransitionBuffer({
            .Buffer = BufferA(),
            .Usage = NVulkan::EVulkanResourceUsage::TransferWrite,
    });

    ASSERT_TRUE(barrier.has_value());
    EXPECT_EQ(barrier->srcAccessMask, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    EXPECT_EQ(barrier->dstAccessMask, VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

TEST(VulkanResourceStateTracker, RejectsInvalidResourceStateRequests) {
    NVulkan::VulkanResourceStateTracker tracker;

    EXPECT_THROW(tracker.ImportBuffer(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE, {}), NCommon::Exception);
    EXPECT_THROW((void)tracker.TransitionBuffer({
                         .Buffer = VK_NULL_HANDLE,
                         .Usage = NVulkan::EVulkanResourceUsage::TransferRead,
                 }),
                 NCommon::Exception);
    EXPECT_THROW((void)tracker.TransitionBuffer({
                         .Buffer = BufferA(),
                         .Usage = NVulkan::EVulkanResourceUsage::TransferRead,
                         .QueueFamilyIndex = 4,
                 }),
                 NCommon::Exception);
}

} // namespace
