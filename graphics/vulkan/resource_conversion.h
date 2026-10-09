#pragma once

#include <vulkan.h>

#include <GraphicsEngine/graphics/image.h>

namespace NVulkan {

[[nodiscard]] VkFormat ToVulkanFormat(NGraphics::EImageFormat format);
[[nodiscard]] VkImageUsageFlags ToVulkanImageUsage(NGraphics::ImageUsageFlags usage);
[[nodiscard]] VkImageAspectFlags ToVulkanImageAspect(NGraphics::ImageAspectFlags aspects);
[[nodiscard]] VkFilter ToVulkanFilter(NGraphics::ESamplerFilter filter);
[[nodiscard]] VkSamplerAddressMode ToVulkanAddressMode(NGraphics::ESamplerAddressMode addressMode);

} // namespace NVulkan
