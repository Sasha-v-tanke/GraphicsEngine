#include "resource_conversion.h"

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {
namespace {

constexpr NGraphics::ImageUsageFlags KnownImageUsageMask =
        NGraphics::ImageUsage(NGraphics::EImageUsage::TransferSource) |
        NGraphics::ImageUsage(NGraphics::EImageUsage::TransferDestination) |
        NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled) |
        NGraphics::ImageUsage(NGraphics::EImageUsage::ColorAttachment) |
        NGraphics::ImageUsage(NGraphics::EImageUsage::DepthStencilAttachment);

constexpr NGraphics::ImageAspectFlags KnownImageAspectMask =
        NGraphics::ImageAspect(NGraphics::EImageAspect::Color) | NGraphics::ImageAspect(NGraphics::EImageAspect::Depth);

} // namespace

VkFormat ToVulkanFormat(NGraphics::EImageFormat format) {
    switch (format) {
    case NGraphics::EImageFormat::R8_UNORM:
        return VK_FORMAT_R8_UNORM;
    case NGraphics::EImageFormat::RG8_UNORM:
        return VK_FORMAT_R8G8_UNORM;
    case NGraphics::EImageFormat::RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case NGraphics::EImageFormat::BGRA8_UNORM:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case NGraphics::EImageFormat::D32_FLOAT:
        return VK_FORMAT_D32_SFLOAT;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Unsupported image format");
}

VkImageUsageFlags ToVulkanImageUsage(NGraphics::ImageUsageFlags usage) {
    if ((usage & ~KnownImageUsageMask) != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Unsupported Vulkan image usage flags");
    }

    VkImageUsageFlags result = 0;

    if ((usage & NGraphics::ImageUsage(NGraphics::EImageUsage::TransferSource)) != 0) {
        result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    if ((usage & NGraphics::ImageUsage(NGraphics::EImageUsage::TransferDestination)) != 0) {
        result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    if ((usage & NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled)) != 0) {
        result |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }

    if ((usage & NGraphics::ImageUsage(NGraphics::EImageUsage::ColorAttachment)) != 0) {
        result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    if ((usage & NGraphics::ImageUsage(NGraphics::EImageUsage::DepthStencilAttachment)) != 0) {
        result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }

    if (result == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan image usage must not be empty");
    }

    return result;
}

VkImageAspectFlags ToVulkanImageAspect(NGraphics::ImageAspectFlags aspects) {
    if ((aspects & ~KnownImageAspectMask) != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Unsupported Vulkan image aspect flags");
    }

    VkImageAspectFlags result = 0;

    if ((aspects & NGraphics::ImageAspect(NGraphics::EImageAspect::Color)) != 0) {
        result |= VK_IMAGE_ASPECT_COLOR_BIT;
    }

    if ((aspects & NGraphics::ImageAspect(NGraphics::EImageAspect::Depth)) != 0) {
        result |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    if (result == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan image aspect mask must not be empty");
    }

    return result;
}

VkFilter ToVulkanFilter(NGraphics::ESamplerFilter filter) {
    switch (filter) {
    case NGraphics::ESamplerFilter::Nearest:
        return VK_FILTER_NEAREST;
    case NGraphics::ESamplerFilter::Linear:
        return VK_FILTER_LINEAR;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Unsupported sampler filter");
}

VkSamplerAddressMode ToVulkanAddressMode(NGraphics::ESamplerAddressMode addressMode) {
    switch (addressMode) {
    case NGraphics::ESamplerAddressMode::Repeat:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NGraphics::ESamplerAddressMode::MirroredRepeat:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NGraphics::ESamplerAddressMode::ClampToEdge:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case NGraphics::ESamplerAddressMode::ClampToBorder:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Unsupported sampler address mode");
}

} // namespace NVulkan
