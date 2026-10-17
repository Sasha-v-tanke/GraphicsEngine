#pragma once

#include <vulkan.h>

#include <GraphicsEngine/graphics/graphics_pipeline.h>
#include <GraphicsEngine/graphics/vulkan/device.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NVulkan {

class VulkanGraphicsPipeline final: public NCommon::NonTransferable {
public:
    VulkanGraphicsPipeline(const VulkanDevice& device,
                           const VulkanPhysicalDeviceSelection& physicalDevice,
                           const NGraphics::GraphicsPipelineDescriptor& descriptor);
    ~VulkanGraphicsPipeline();

    [[nodiscard]] VkPipeline GetHandle() const noexcept;
    [[nodiscard]] VkPipelineLayout GetLayout() const noexcept;

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace NVulkan
