#include "graphics_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NGraphics {

namespace {

constexpr std::uint64_t HASH_OFFSET = 14695981039346656037ULL;
constexpr std::uint64_t HASH_PRIME = 1099511628211ULL;
constexpr std::uint32_t SPIR_V_MAGIC = 0x07230203U;

constexpr ColorComponentFlags KNOWN_COLOR_COMPONENT_MASK =
        ColorComponent(EColorComponent::RED) | ColorComponent(EColorComponent::GREEN) |
        ColorComponent(EColorComponent::BLUE) | ColorComponent(EColorComponent::ALPHA);

template<typename T>
void HashIntegral(std::uint64_t& hash, T value) noexcept {
    using UnsignedType = std::make_unsigned_t<T>;
    UnsignedType bits = static_cast<UnsignedType>(value);

    for (std::size_t index = 0; index < sizeof(UnsignedType); ++index) {
        hash ^= static_cast<std::uint8_t>(bits & 0xFFU);
        hash *= HASH_PRIME;
        bits >>= 8U;
    }
}

template<typename Enum>
void HashEnum(std::uint64_t& hash, Enum value) noexcept {
    HashIntegral(hash, static_cast<std::underlying_type_t<Enum>>(value));
}

void HashString(std::uint64_t& hash, const std::string& value) noexcept {
    HashIntegral(hash, value.size());

    for (const char character: value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= HASH_PRIME;
    }
}

void HashShader(std::uint64_t& hash, const Shader& shader) noexcept {
    HashEnum(hash, shader.GetStage());
    HashEnum(hash, shader.GetArtifact().Format);
    HashString(hash, shader.GetEntryPoint());
    HashIntegral(hash, shader.GetArtifact().Words.size());

    for (const std::uint32_t word: shader.GetArtifact().Words) {
        HashIntegral(hash, word);
    }
}

[[nodiscard]] std::uint32_t GetVertexFormatSize(EVertexFormat format) noexcept {
    switch (format) {
    case EVertexFormat::FLOAT32:
    case EVertexFormat::UINT32:
        return 4;
    case EVertexFormat::FLOAT32_2:
    case EVertexFormat::UINT32_2:
        return 8;
    case EVertexFormat::FLOAT32_3:
    case EVertexFormat::UINT32_3:
        return 12;
    case EVertexFormat::FLOAT32_4:
    case EVertexFormat::UINT32_4:
        return 16;
    }

    return 0;
}

[[nodiscard]] bool IsColorFormat(EPixelFormat format) noexcept {
    switch (format) {
    case EPixelFormat::RGBA8_UNORM:
    case EPixelFormat::BGRA8_UNORM:
    case EPixelFormat::RGBA8_SRGB:
    case EPixelFormat::BGRA8_SRGB:
    case EPixelFormat::RGBA16_FLOAT:
        return true;
    case EPixelFormat::UNDEFINED:
    case EPixelFormat::D32_FLOAT:
    case EPixelFormat::D24_UNORM_S8_UINT:
        return false;
    }

    return false;
}

[[nodiscard]] bool IsDepthFormat(EPixelFormat format) noexcept {
    return format == EPixelFormat::D32_FLOAT || format == EPixelFormat::D24_UNORM_S8_UINT;
}

void ValidateShader(const Shader& shader) {
    if (shader.GetEntryPoint().empty()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Shader entry point must not be empty");
    }

    if (shader.GetArtifact().Format != EShaderArtifactFormat::SPIR_V) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Shader artifact format is not supported");
    }

    const std::vector<std::uint32_t>& words = shader.GetArtifact().Words;

    if (words.size() < 5 || words.front() != SPIR_V_MAGIC) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Shader artifact is not valid SPIR-V");
    }
}

void ValidateVertexLayout(const VertexLayoutDescriptor& layout) {
    std::unordered_map<std::uint32_t, std::uint32_t> strides;

    for (const VertexBindingDescriptor& binding: layout.Bindings) {
        if (binding.StrideBytes == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vertex binding stride must be greater than zero");
        }

        if (!strides.emplace(binding.Binding, binding.StrideBytes).second) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vertex binding is duplicated");
        }
    }

    std::unordered_set<std::uint32_t> locations;

    for (const VertexAttributeDescriptor& attribute: layout.Attributes) {
        if (!locations.insert(attribute.Location).second) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vertex attribute location is duplicated");
        }

        const auto binding = strides.find(attribute.Binding);

        if (binding == strides.end()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vertex attribute references unknown binding");
        }

        const std::uint32_t formatSize = GetVertexFormatSize(attribute.Format);

        if (formatSize == 0 || formatSize > binding->second || attribute.OffsetBytes > binding->second - formatSize) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vertex attribute exceeds binding stride");
        }
    }
}

void ValidateAttachments(const GraphicsPipelineDescriptor& descriptor) {
    if (descriptor.ColorAttachmentFormats.empty() && descriptor.DepthAttachmentFormat == EPixelFormat::UNDEFINED) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline must declare an attachment format");
    }

    if (descriptor.ColorBlendAttachments.size() != descriptor.ColorAttachmentFormats.size()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Color blend state count must match color attachment count");
    }

    for (const EPixelFormat format: descriptor.ColorAttachmentFormats) {
        if (!IsColorFormat(format)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Color attachment format is not color-capable");
        }
    }

    for (const BlendAttachmentDescriptor& blend: descriptor.ColorBlendAttachments) {
        if ((blend.WriteMask & ~KNOWN_COLOR_COMPONENT_MASK) != 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Color write mask contains unknown flags");
        }
    }

    if (descriptor.DepthAttachmentFormat != EPixelFormat::UNDEFINED &&
        !IsDepthFormat(descriptor.DepthAttachmentFormat)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Depth attachment format is not depth-capable");
    }

    if ((descriptor.DepthState.TestEnabled || descriptor.DepthState.WriteEnabled) &&
        descriptor.DepthAttachmentFormat == EPixelFormat::UNDEFINED) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Depth state requires a depth attachment format");
    }
}

void ValidateSampleCount(ESampleCount samples) {
    switch (samples) {
    case ESampleCount::X1:
    case ESampleCount::X2:
    case ESampleCount::X4:
    case ESampleCount::X8:
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline sample count is invalid");
}

} // namespace

void ValidateGraphicsPipelineDescriptor(const GraphicsPipelineDescriptor& descriptor) {
    bool hasVertexShader = false;
    bool hasFragmentShader = false;

    for (const Shader& shader: descriptor.Shaders) {
        ValidateShader(shader);

        switch (shader.GetStage()) {
        case EShaderStage::VERTEX:
            if (hasVertexShader) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline has duplicate vertex shader");
            }

            hasVertexShader = true;
            break;
        case EShaderStage::FRAGMENT:
            if (hasFragmentShader) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                      "Graphics pipeline has duplicate fragment shader");
            }

            hasFragmentShader = true;
            break;
        case EShaderStage::COMPUTE:
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline cannot contain compute shader");
        }
    }

    if (!hasVertexShader || !hasFragmentShader) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Graphics pipeline requires one vertex and one fragment shader");
    }

    ValidateVertexLayout(descriptor.VertexLayout);
    ValidateAttachments(descriptor);
    ValidateSampleCount(descriptor.Samples);
}

std::uint64_t HashGraphicsPipelineDescriptor(const GraphicsPipelineDescriptor& descriptor) noexcept {
    std::uint64_t hash = HASH_OFFSET;

    HashIntegral(hash, descriptor.Shaders.size());

    for (const Shader& shader: descriptor.Shaders) {
        HashShader(hash, shader);
    }

    HashIntegral(hash, descriptor.VertexLayout.Bindings.size());

    for (const VertexBindingDescriptor& binding: descriptor.VertexLayout.Bindings) {
        HashIntegral(hash, binding.Binding);
        HashIntegral(hash, binding.StrideBytes);
        HashEnum(hash, binding.InputRate);
    }

    HashIntegral(hash, descriptor.VertexLayout.Attributes.size());

    for (const VertexAttributeDescriptor& attribute: descriptor.VertexLayout.Attributes) {
        HashIntegral(hash, attribute.Location);
        HashIntegral(hash, attribute.Binding);
        HashEnum(hash, attribute.Format);
        HashIntegral(hash, attribute.OffsetBytes);
    }

    HashEnum(hash, descriptor.Topology);
    HashEnum(hash, descriptor.RasterState.PolygonMode);
    HashEnum(hash, descriptor.RasterState.CullMode);
    HashEnum(hash, descriptor.RasterState.FrontFace);
    HashIntegral(hash, descriptor.DepthState.TestEnabled);
    HashIntegral(hash, descriptor.DepthState.WriteEnabled);
    HashEnum(hash, descriptor.DepthState.CompareOperation);
    HashIntegral(hash, descriptor.ColorAttachmentFormats.size());

    for (const EPixelFormat format: descriptor.ColorAttachmentFormats) {
        HashEnum(hash, format);
    }

    HashIntegral(hash, descriptor.ColorBlendAttachments.size());

    for (const BlendAttachmentDescriptor& blend: descriptor.ColorBlendAttachments) {
        HashIntegral(hash, blend.Enabled);
        HashEnum(hash, blend.SourceColorFactor);
        HashEnum(hash, blend.DestinationColorFactor);
        HashEnum(hash, blend.ColorOperation);
        HashEnum(hash, blend.SourceAlphaFactor);
        HashEnum(hash, blend.DestinationAlphaFactor);
        HashEnum(hash, blend.AlphaOperation);
        HashIntegral(hash, blend.WriteMask);
    }

    HashEnum(hash, descriptor.DepthAttachmentFormat);
    HashEnum(hash, descriptor.Samples);

    return hash;
}

} // namespace NGraphics
