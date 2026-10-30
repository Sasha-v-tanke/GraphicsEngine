#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <graphics/vulkan/rendering_scope.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>

namespace {
class RenderingScopeTest: public testing::Test {
protected:
    void SetUp() override {
        OriginalBegin = vkCmdBeginRendering;
        OriginalEnd = vkCmdEndRendering;
        vkCmdBeginRendering = Begin;
        vkCmdEndRendering = End;
        Begins = 0;
        Ends = 0;
        Attachments.clear();
    }
    void TearDown() override {
        vkCmdBeginRendering = OriginalBegin;
        vkCmdEndRendering = OriginalEnd;
    }
    static VKAPI_ATTR void VKAPI_CALL Begin(VkCommandBuffer, const VkRenderingInfo* info) {
        ++Begins;
        Area = info->renderArea;
        Flags = info->flags;
        Layers = info->layerCount;
        Attachments.assign(info->pColorAttachments, info->pColorAttachments + info->colorAttachmentCount);
    }
    static VKAPI_ATTR void VKAPI_CALL End(VkCommandBuffer) {
        ++Ends;
    }
    static VkCommandBuffer Commands() {
        return reinterpret_cast<VkCommandBuffer>(std::uintptr_t{1});
    }
    static VkImageView View() {
        return reinterpret_cast<VkImageView>(std::uintptr_t{2});
    }
    inline static int Begins = 0;
    inline static int Ends = 0;
    inline static VkRect2D Area{};
    inline static VkRenderingFlags Flags = 0;
    inline static std::uint32_t Layers = 0;
    inline static std::vector<VkRenderingAttachmentInfo> Attachments;
    PFN_vkCmdBeginRendering OriginalBegin = nullptr;
    PFN_vkCmdEndRendering OriginalEnd = nullptr;
};

TEST_F(RenderingScopeTest, BindsRuntimeAttachmentsAndBalancesScopeOnFailure) {
    const std::array colors{NVulkan::VulkanColorAttachment{.View = View(), .Clear = {.float32 = {1, 0, 0, 1}}}};
    EXPECT_THROW(([&] {
                     const NVulkan::VulkanRenderingScope scope{Commands(), {.extent = {64, 32}}, colors};
                     ASSERT_EQ(Begins, 1);
                     EXPECT_EQ(Ends, 0);
                     throw std::runtime_error{"recording failed"};
                 }()),
                 std::runtime_error);
    EXPECT_EQ(Ends, 1);
    ASSERT_EQ(Attachments.size(), 1);
    EXPECT_EQ(Attachments[0].imageView, View());
    EXPECT_EQ(Attachments[0].imageLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(Attachments[0].loadOp, VK_ATTACHMENT_LOAD_OP_CLEAR);
    EXPECT_EQ(Attachments[0].storeOp, VK_ATTACHMENT_STORE_OP_STORE);
    EXPECT_FLOAT_EQ(Attachments[0].clearValue.color.float32[0], 1);
    EXPECT_EQ(Area.extent.width, 64U);
    EXPECT_EQ(Layers, 1U);
    EXPECT_EQ(Flags, 0U);
}

TEST_F(RenderingScopeTest, SupportsSecondaryContentsAndNewExtentWithoutPersistentObjects) {
    const std::array colors{NVulkan::VulkanColorAttachment{.View = View()}};
    for (const std::uint32_t size: {32U, 96U}) {
        const NVulkan::VulkanRenderingScope scope{Commands(),
                                                  {.extent = {size, size}},
                                                  colors,
                                                  NVulkan::ERenderingContents::SECONDARY_COMMAND_BUFFERS};
        EXPECT_EQ(Area.extent.width, size);
        EXPECT_EQ(Flags, VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
    }
    EXPECT_EQ(Begins, 2);
    EXPECT_EQ(Ends, 2);
}

TEST_F(RenderingScopeTest, InvalidInputDoesNotBeginRendering) {
    const std::array colors{NVulkan::VulkanColorAttachment{.View = View()}};
    EXPECT_THROW((NVulkan::VulkanRenderingScope{VK_NULL_HANDLE, {.extent = {64, 32}}, colors}), NCommon::Exception);
    EXPECT_THROW((NVulkan::VulkanRenderingScope{Commands(), {}, colors}), NCommon::Exception);
    EXPECT_THROW((NVulkan::VulkanRenderingScope{Commands(), {.extent = {64, 32}}, {}}), NCommon::Exception);
    const std::array invalid{NVulkan::VulkanColorAttachment{}};
    EXPECT_THROW((NVulkan::VulkanRenderingScope{Commands(), {.extent = {64, 32}}, invalid}), NCommon::Exception);
    EXPECT_EQ(Begins, 0);
    EXPECT_EQ(Ends, 0);
}
} // namespace
