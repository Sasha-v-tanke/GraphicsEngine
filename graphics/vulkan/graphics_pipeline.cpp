#include "graphics_pipeline.h"

#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

namespace {

template<typename T, std::size_t N, typename E>
T Convert(E value, const std::array<T, N>& values) {
    const auto index = static_cast<std::size_t>(value);
    if (index >= values.size()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid graphics pipeline enum value");
    }
    return values[index];
}

VkFormat Format(NGraphics::EPixelFormat value) {
    return Convert(value,
                   std::array{VK_FORMAT_UNDEFINED,
                              VK_FORMAT_R8G8B8A8_UNORM,
                              VK_FORMAT_B8G8R8A8_UNORM,
                              VK_FORMAT_R8G8B8A8_SRGB,
                              VK_FORMAT_B8G8R8A8_SRGB,
                              VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_FORMAT_D32_SFLOAT,
                              VK_FORMAT_D24_UNORM_S8_UINT});
}

VkFormat Format(NGraphics::EVertexFormat value) {
    return Convert(value,
                   std::array{VK_FORMAT_R32_SFLOAT,
                              VK_FORMAT_R32G32_SFLOAT,
                              VK_FORMAT_R32G32B32_SFLOAT,
                              VK_FORMAT_R32G32B32A32_SFLOAT,
                              VK_FORMAT_R32_UINT,
                              VK_FORMAT_R32G32_UINT,
                              VK_FORMAT_R32G32B32_UINT,
                              VK_FORMAT_R32G32B32A32_UINT});
}

VkBlendFactor BlendFactor(NGraphics::EBlendFactor value) {
    return Convert(value,
                   std::array{VK_BLEND_FACTOR_ZERO,
                              VK_BLEND_FACTOR_ONE,
                              VK_BLEND_FACTOR_SRC_COLOR,
                              VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
                              VK_BLEND_FACTOR_DST_COLOR,
                              VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
                              VK_BLEND_FACTOR_SRC_ALPHA,
                              VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                              VK_BLEND_FACTOR_DST_ALPHA,
                              VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA});
}

VkBlendOp BlendOperation(NGraphics::EBlendOperation value) {
    return Convert(value,
                   std::array{VK_BLEND_OP_ADD,
                              VK_BLEND_OP_SUBTRACT,
                              VK_BLEND_OP_REVERSE_SUBTRACT,
                              VK_BLEND_OP_MIN,
                              VK_BLEND_OP_MAX});
}

void CheckResult(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "{} failed with VkResult {}",
                              operation,
                              static_cast<int>(result));
    }
}

class ShaderModules final {
public:
    explicit ShaderModules(VkDevice device)
        : m_device(device) {
    }
    ~ShaderModules() {
        for (VkShaderModule module: Handles) {
            vkDestroyShaderModule(m_device, module, nullptr);
        }
    }
    std::vector<VkShaderModule> Handles;

private:
    VkDevice m_device;
};

void ValidateShaderInterface(const NGraphics::Shader& shader) {
    const auto& words = shader.GetArtifact()->GetWords();
    bool foundEntryPoint = false;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = static_cast<std::size_t>(words[offset] >> 16U);
        const auto opcode = words[offset] & 0xffffU;
        if (count == 0 || count > words.size() - offset) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Malformed SPIR-V instruction");
        }
        if (opcode == 15U && count >= 4) {
            const auto* name = reinterpret_cast<const char*>(&words[offset + 3]);
            const auto capacity = (count - 3) * sizeof(std::uint32_t);
            const auto* end = static_cast<const char*>(std::memchr(name, 0, capacity));
            if (end == nullptr) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Malformed SPIR-V entry point");
            }
            const std::uint32_t expectedModel =
                    shader.GetArtifact()->GetStage() == NResources::EShaderStage::VERTEX ? 0U : 4U;
            if (std::string_view{name, static_cast<std::size_t>(end - name)} == shader.GetEntryPoint() &&
                words[offset + 1] == expectedModel) {
                foundEntryPoint = true;
            }
        }
        if ((opcode == 71U && count >= 3 && (words[offset + 2] == 33U || words[offset + 2] == 34U)) ||
            (opcode == 59U && count >= 4 && words[offset + 3] == 9U)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED,
                                  "Shader bindings and push constants require a pipeline layout contract");
        }
        offset += count;
    }
    if (!foundEntryPoint) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Shader entry point does not match its stage");
    }
}

void ValidateDevice(const VulkanPhysicalDeviceSelection& physicalDevice,
                    const NGraphics::GraphicsPipelineDescriptor& descriptor) {
    if (physicalDevice.Handle == VK_NULL_HANDLE || !physicalDevice.Capabilities.Features.DynamicRendering) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Graphics pipelines require dynamic rendering");
    }
    if (descriptor.RasterState.PolygonMode != NGraphics::EPolygonMode::FILL) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Non-solid polygon mode is not enabled on VulkanDevice");
    }
    if (!descriptor.ColorBlendAttachments.empty()) {
        for (const auto& blend: descriptor.ColorBlendAttachments) {
            if (blend != descriptor.ColorBlendAttachments.front()) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Independent blend is not enabled on VulkanDevice");
            }
        }
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice.Handle, &properties);
    const auto& limits = properties.limits;
    if (descriptor.ColorAttachmentFormats.size() > limits.maxColorAttachments ||
        descriptor.VertexLayout.Bindings.size() > limits.maxVertexInputBindings ||
        descriptor.VertexLayout.Attributes.size() > limits.maxVertexInputAttributes) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Graphics pipeline exceeds Vulkan device limits");
    }
    const auto samples = static_cast<VkSampleCountFlags>(descriptor.Samples);
    if ((!descriptor.ColorAttachmentFormats.empty() && (limits.framebufferColorSampleCounts & samples) == 0) ||
        (descriptor.DepthAttachmentFormat != NGraphics::EPixelFormat::UNDEFINED &&
         (limits.framebufferDepthSampleCounts & samples) == 0)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Graphics pipeline sample count is unsupported");
    }
    auto validateAttachment = [&](NGraphics::EPixelFormat format, VkImageUsageFlags usage, bool blending) {
        VkImageFormatProperties imageProperties{};
        const VkResult result = vkGetPhysicalDeviceImageFormatProperties(physicalDevice.Handle,
                                                                         Format(format),
                                                                         VK_IMAGE_TYPE_2D,
                                                                         VK_IMAGE_TILING_OPTIMAL,
                                                                         usage,
                                                                         0,
                                                                         &imageProperties);
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice.Handle, Format(format), &formatProperties);
        if (result != VK_SUCCESS || (imageProperties.sampleCounts & samples) == 0 ||
            (blending &&
             (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) == 0)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED,
                                  "Pipeline attachment format/sample/blend is unsupported");
        }
    };
    for (std::size_t index = 0; index < descriptor.ColorAttachmentFormats.size(); ++index) {
        validateAttachment(descriptor.ColorAttachmentFormats[index],
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                           descriptor.ColorBlendAttachments[index].Enabled);
    }
    if (descriptor.DepthAttachmentFormat != NGraphics::EPixelFormat::UNDEFINED) {
        validateAttachment(descriptor.DepthAttachmentFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, false);
    }
    for (const auto& binding: descriptor.VertexLayout.Bindings) {
        if (binding.Binding >= limits.maxVertexInputBindings ||
            binding.StrideBytes > limits.maxVertexInputBindingStride) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Vertex binding exceeds Vulkan device limits");
        }
    }
    for (const auto& attribute: descriptor.VertexLayout.Attributes) {
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice.Handle, Format(attribute.Format), &formatProperties);
        if (attribute.Location >= limits.maxVertexInputAttributes ||
            attribute.OffsetBytes > limits.maxVertexInputAttributeOffset ||
            (formatProperties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Vertex attribute is unsupported on Vulkan device");
        }
    }
}

VkPipelineInputAssemblyStateCreateInfo MakeInputAssembly(NGraphics::EPrimitiveTopology topology) {
    return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .topology = Convert(topology,
                                std::array{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                           VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                                           VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                           VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
                                           VK_PRIMITIVE_TOPOLOGY_POINT_LIST}),
            .primitiveRestartEnable = VK_FALSE,
    };
}

VkPipelineViewportStateCreateInfo MakeViewport() {
    return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .viewportCount = 1,
            .pViewports = nullptr,
            .scissorCount = 1,
            .pScissors = nullptr,
    };
}

VkPipelineRasterizationStateCreateInfo MakeRasterization(const NGraphics::RasterStateDescriptor& rasterState) {
    return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = Convert(rasterState.CullMode,
                                std::array<VkCullModeFlags, 4>{VK_CULL_MODE_NONE,
                                                               VK_CULL_MODE_FRONT_BIT,
                                                               VK_CULL_MODE_BACK_BIT,
                                                               VK_CULL_MODE_FRONT_AND_BACK}),
            .frontFace = rasterState.FrontFace == NGraphics::EFrontFace::COUNTER_CLOCKWISE
                               ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                               : VK_FRONT_FACE_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .depthBiasConstantFactor = 0.0F,
            .depthBiasClamp = 0.0F,
            .depthBiasSlopeFactor = 0.0F,
            .lineWidth = 1.0F,
    };
}

VkPipelineMultisampleStateCreateInfo MakeMultisample(NGraphics::ESampleCount samples) {
    return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .rasterizationSamples = static_cast<VkSampleCountFlagBits>(samples),
            .sampleShadingEnable = VK_FALSE,
            .minSampleShading = 0.0F,
            .pSampleMask = nullptr,
            .alphaToCoverageEnable = VK_FALSE,
            .alphaToOneEnable = VK_FALSE,
    };
}

VkPipelineDepthStencilStateCreateInfo MakeDepthStencil(const NGraphics::DepthStateDescriptor& depthState) {
    return {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthTestEnable = depthState.TestEnabled,
            .depthWriteEnable = depthState.WriteEnabled,
            .depthCompareOp = Convert(depthState.CompareOperation,
                                      std::array{VK_COMPARE_OP_NEVER,
                                                 VK_COMPARE_OP_LESS,
                                                 VK_COMPARE_OP_EQUAL,
                                                 VK_COMPARE_OP_LESS_OR_EQUAL,
                                                 VK_COMPARE_OP_GREATER,
                                                 VK_COMPARE_OP_NOT_EQUAL,
                                                 VK_COMPARE_OP_GREATER_OR_EQUAL,
                                                 VK_COMPARE_OP_ALWAYS}),
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE,
            .front = {},
            .back = {},
            .minDepthBounds = 0.0F,
            .maxDepthBounds = 1.0F,
    };
}

} // namespace

VulkanGraphicsPipeline::VulkanGraphicsPipeline(const VulkanDevice& device,
                                               const VulkanPhysicalDeviceSelection& physicalDevice,
                                               const NGraphics::GraphicsPipelineDescriptor& descriptor,
                                               VkPipelineCache cache)
    : m_device(device.GetHandle()) {
    NGraphics::ValidateGraphicsPipelineDescriptor(descriptor);
    ValidateDevice(physicalDevice, descriptor);
    for (const auto& shader: descriptor.Shaders) {
        ValidateShaderInterface(shader);
    }
    ShaderModules modules{m_device};
    modules.Handles.reserve(descriptor.Shaders.size());
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    stages.reserve(descriptor.Shaders.size());
    for (const auto& shader: descriptor.Shaders) {
        const auto& words = shader.GetArtifact()->GetWords();
        const VkShaderModuleCreateInfo moduleInfo{
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .codeSize = words.size() * sizeof(std::uint32_t),
                .pCode = words.data(),
        };
        VkShaderModule module = VK_NULL_HANDLE;
        CheckResult(vkCreateShaderModule(m_device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
        modules.Handles.push_back(module);
        stages.push_back({
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = shader.GetArtifact()->GetStage() == NResources::EShaderStage::VERTEX
                               ? VK_SHADER_STAGE_VERTEX_BIT
                               : VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = module,
                .pName = shader.GetEntryPoint().c_str(),
                .pSpecializationInfo = nullptr,
        });
    }
    std::vector<VkVertexInputBindingDescription> bindings;
    bindings.reserve(descriptor.VertexLayout.Bindings.size());
    for (const auto& binding: descriptor.VertexLayout.Bindings) {
        bindings.push_back({binding.Binding,
                            binding.StrideBytes,
                            binding.InputRate == NGraphics::EVertexInputRate::VERTEX ? VK_VERTEX_INPUT_RATE_VERTEX
                                                                                     : VK_VERTEX_INPUT_RATE_INSTANCE});
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(descriptor.VertexLayout.Attributes.size());
    for (const auto& attribute: descriptor.VertexLayout.Attributes) {
        attributes.push_back({attribute.Location, attribute.Binding, Format(attribute.Format), attribute.OffsetBytes});
    }
    const VkPipelineVertexInputStateCreateInfo vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size()),
            .pVertexBindingDescriptions = bindings.data(),
            .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size()),
            .pVertexAttributeDescriptions = attributes.data(),
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly = MakeInputAssembly(descriptor.Topology);
    const VkPipelineViewportStateCreateInfo viewport = MakeViewport();
    const VkPipelineRasterizationStateCreateInfo raster = MakeRasterization(descriptor.RasterState);
    const VkPipelineMultisampleStateCreateInfo multisample = MakeMultisample(descriptor.Samples);
    const VkPipelineDepthStencilStateCreateInfo depth = MakeDepthStencil(descriptor.DepthState);
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
    std::vector<VkFormat> colorFormats;
    blendAttachments.reserve(descriptor.ColorAttachmentFormats.size());
    colorFormats.reserve(descriptor.ColorAttachmentFormats.size());
    for (std::size_t i = 0; i < descriptor.ColorAttachmentFormats.size(); ++i) {
        colorFormats.push_back(Format(descriptor.ColorAttachmentFormats[i]));
        const auto blend = descriptor.ColorBlendAttachments.empty() ? NGraphics::BlendAttachmentDescriptor{}
                                                                    : descriptor.ColorBlendAttachments[i];
        blendAttachments.push_back({blend.Enabled,
                                    BlendFactor(blend.SourceColorFactor),
                                    BlendFactor(blend.DestinationColorFactor),
                                    BlendOperation(blend.ColorOperation),
                                    BlendFactor(blend.SourceAlphaFactor),
                                    BlendFactor(blend.DestinationAlphaFactor),
                                    BlendOperation(blend.AlphaOperation),
                                    blend.WriteMask});
    }
    const VkPipelineColorBlendStateCreateInfo blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .logicOpEnable = VK_FALSE,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = static_cast<std::uint32_t>(blendAttachments.size()),
            .pAttachments = blendAttachments.data(),
            .blendConstants = {},
    };
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
            .pDynamicStates = dynamicStates.data(),
    };
    const VkPipelineRenderingCreateInfo rendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext = nullptr,
            .viewMask = 0,
            .colorAttachmentCount = static_cast<std::uint32_t>(colorFormats.size()),
            .pColorAttachmentFormats = colorFormats.data(),
            .depthAttachmentFormat = Format(descriptor.DepthAttachmentFormat),
            .stencilAttachmentFormat = descriptor.DepthAttachmentFormat == NGraphics::EPixelFormat::D24_UNORM_S8_UINT
                                             ? VK_FORMAT_D24_UNORM_S8_UINT
                                             : VK_FORMAT_UNDEFINED,
    };
    const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 0,
            .pSetLayouts = nullptr,
            .pushConstantRangeCount = 0,
            .pPushConstantRanges = nullptr,
    };
    CheckResult(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_layout), "vkCreatePipelineLayout");
    const VkGraphicsPipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &rendering,
            .flags = 0,
            .stageCount = static_cast<std::uint32_t>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &assembly,
            .pTessellationState = nullptr,
            .pViewportState = &viewport,
            .pRasterizationState = &raster,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depth,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamic,
            .layout = m_layout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = -1,
    };
    const VkResult result = vkCreateGraphicsPipelines(m_device, cache, 1, &pipelineInfo, nullptr, &m_pipeline);
    if (result != VK_SUCCESS) {
        if (m_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
        }
        vkDestroyPipelineLayout(m_device, m_layout, nullptr);
        CheckResult(result, "vkCreateGraphicsPipelines");
    }
}

VulkanGraphicsPipeline::~VulkanGraphicsPipeline() {
    vkDestroyPipeline(m_device, m_pipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_layout, nullptr);
}

VkPipeline VulkanGraphicsPipeline::GetHandle() const noexcept {
    return m_pipeline;
}
VkPipelineLayout VulkanGraphicsPipeline::GetLayout() const noexcept {
    return m_layout;
}

} // namespace NVulkan
