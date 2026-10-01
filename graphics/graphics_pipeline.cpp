#include "graphics_pipeline.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include <GraphicsEngine/resources/shader_artifact_loader.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NGraphics {

namespace {

constexpr std::uint64_t HASH_OFFSET = 14695981039346656037ULL;
constexpr std::uint64_t HASH_PRIME = 1099511628211ULL;

constexpr ColorComponentFlags KNOWN_COLOR_COMPONENT_MASK =
        ColorComponent(EColorComponent::RED) | ColorComponent(EColorComponent::GREEN) |
        ColorComponent(EColorComponent::BLUE) | ColorComponent(EColorComponent::ALPHA);

template<typename T>
void HashIntegral(std::uint64_t& hash, T value) noexcept {
    if constexpr (std::is_same_v<std::remove_cv_t<T>, bool>) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= HASH_PRIME;
    } else {
        using UnsignedType = std::make_unsigned_t<T>;
        UnsignedType bits = static_cast<UnsignedType>(value);

        for (std::size_t index = 0; index < sizeof(UnsignedType); ++index) {
            hash ^= static_cast<std::uint8_t>(bits & 0xFFU);
            hash *= HASH_PRIME;
            bits >>= 8U;
        }
    }
}

template<typename Enum>
void HashEnum(std::uint64_t& hash, Enum value) noexcept {
    HashIntegral(hash, static_cast<std::underlying_type_t<Enum>>(value));
}

void HashString(std::uint64_t& hash, std::string_view value) noexcept {
    HashIntegral(hash, value.size());

    for (const char character: value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= HASH_PRIME;
    }
}

void HashShader(std::uint64_t& hash, const Shader& shader) noexcept {
    const NResources::ResourceIdentity& identity = shader.GetArtifactIdentity();

    HashString(hash, identity.GetResourceClass());
    HashString(hash, identity.GetKey());
    HashString(hash, shader.GetEntryPoint());

    const std::shared_ptr<const NResources::ShaderArtifact>& artifact = shader.GetArtifact();

    HashIntegral(hash, artifact != nullptr);

    if (artifact == nullptr) {
        return;
    }

    HashEnum(hash, artifact->GetStage());
    HashIntegral(hash, artifact->GetWords().size());

    for (const std::uint32_t word: artifact->GetWords()) {
        HashIntegral(hash, word);
    }
}

struct CanonicalShaderStages {
    const Shader* Vertex = nullptr;
    const Shader* Fragment = nullptr;
    bool IsCanonical = true;
};

[[nodiscard]] CanonicalShaderStages GetCanonicalShaderStages(const std::vector<Shader>& shaders) noexcept {
    CanonicalShaderStages stages;

    for (const Shader& shader: shaders) {
        const std::shared_ptr<const NResources::ShaderArtifact>& artifact = shader.GetArtifact();

        if (artifact == nullptr) {
            stages.IsCanonical = false;
            return stages;
        }

        switch (artifact->GetStage()) {
        case NResources::EShaderStage::VERTEX:
            if (stages.Vertex != nullptr) {
                stages.IsCanonical = false;
                return stages;
            }

            stages.Vertex = &shader;
            break;
        case NResources::EShaderStage::FRAGMENT:
            if (stages.Fragment != nullptr) {
                stages.IsCanonical = false;
                return stages;
            }

            stages.Fragment = &shader;
            break;
        case NResources::EShaderStage::COMPUTE:
        default:
            stages.IsCanonical = false;
            return stages;
        }
    }

    return stages;
}

[[nodiscard]] bool EqualOptionalShader(const Shader* left, const Shader* right) noexcept {
    if (left == nullptr || right == nullptr) {
        return left == right;
    }

    return *left == *right;
}

void HashOptionalShader(std::uint64_t& hash, const Shader* shader) noexcept {
    HashIntegral(hash, shader != nullptr);

    if (shader != nullptr) {
        HashShader(hash, *shader);
    }
}

[[nodiscard]] bool EqualShaderSequence(const std::vector<Shader>& left, const std::vector<Shader>& right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool HasUniqueBindings(const std::vector<VertexBindingDescriptor>& bindings) noexcept {
    for (std::size_t left = 0; left < bindings.size(); ++left) {
        for (std::size_t right = left + 1; right < bindings.size(); ++right) {
            if (bindings[left].Binding == bindings[right].Binding) {
                return false;
            }
        }
    }

    return true;
}

[[nodiscard]] bool HasUniqueAttributeLocations(const std::vector<VertexAttributeDescriptor>& attributes) noexcept {
    for (std::size_t left = 0; left < attributes.size(); ++left) {
        for (std::size_t right = left + 1; right < attributes.size(); ++right) {
            if (attributes[left].Location == attributes[right].Location) {
                return false;
            }
        }
    }

    return true;
}

[[nodiscard]] bool IsCanonicalVertexLayout(const VertexLayoutDescriptor& layout) noexcept {
    return HasUniqueBindings(layout.Bindings) && HasUniqueAttributeLocations(layout.Attributes);
}

[[nodiscard]] const VertexBindingDescriptor* FindBinding(const std::vector<VertexBindingDescriptor>& bindings,
                                                         std::uint32_t binding) noexcept {
    for (const VertexBindingDescriptor& candidate: bindings) {
        if (candidate.Binding == binding) {
            return &candidate;
        }
    }

    return nullptr;
}

[[nodiscard]] const VertexAttributeDescriptor* FindAttribute(const std::vector<VertexAttributeDescriptor>& attributes,
                                                             std::uint32_t location) noexcept {
    for (const VertexAttributeDescriptor& candidate: attributes) {
        if (candidate.Location == location) {
            return &candidate;
        }
    }

    return nullptr;
}

void HashVertexBinding(std::uint64_t& hash, const VertexBindingDescriptor& binding) noexcept {
    HashIntegral(hash, binding.Binding);
    HashIntegral(hash, binding.StrideBytes);
    HashEnum(hash, binding.InputRate);
}

void HashVertexAttribute(std::uint64_t& hash, const VertexAttributeDescriptor& attribute) noexcept {
    HashIntegral(hash, attribute.Location);
    HashIntegral(hash, attribute.Binding);
    HashEnum(hash, attribute.Format);
    HashIntegral(hash, attribute.OffsetBytes);
}

[[nodiscard]] const VertexBindingDescriptor* FindBindingByRank(const std::vector<VertexBindingDescriptor>& bindings,
                                                               std::size_t rank) noexcept {
    for (const VertexBindingDescriptor& candidate: bindings) {
        std::size_t lowerCount = 0;

        for (const VertexBindingDescriptor& other: bindings) {
            lowerCount += static_cast<std::size_t>(other.Binding < candidate.Binding);
        }

        if (lowerCount == rank) {
            return &candidate;
        }
    }

    return nullptr;
}

[[nodiscard]] const VertexAttributeDescriptor*
FindAttributeByRank(const std::vector<VertexAttributeDescriptor>& attributes, std::size_t rank) noexcept {
    for (const VertexAttributeDescriptor& candidate: attributes) {
        std::size_t lowerCount = 0;

        for (const VertexAttributeDescriptor& other: attributes) {
            lowerCount += static_cast<std::size_t>(other.Location < candidate.Location);
        }

        if (lowerCount == rank) {
            return &candidate;
        }
    }

    return nullptr;
}

void HashVertexLayout(std::uint64_t& hash, const VertexLayoutDescriptor& layout) noexcept {
    const bool canonical = IsCanonicalVertexLayout(layout);

    HashIntegral(hash, canonical);
    HashIntegral(hash, layout.Bindings.size());

    if (canonical) {
        for (std::size_t rank = 0; rank < layout.Bindings.size(); ++rank) {
            HashVertexBinding(hash, *FindBindingByRank(layout.Bindings, rank));
        }
    } else {
        for (const VertexBindingDescriptor& binding: layout.Bindings) {
            HashVertexBinding(hash, binding);
        }
    }

    HashIntegral(hash, layout.Attributes.size());

    if (canonical) {
        for (std::size_t rank = 0; rank < layout.Attributes.size(); ++rank) {
            HashVertexAttribute(hash, *FindAttributeByRank(layout.Attributes, rank));
        }
    } else {
        for (const VertexAttributeDescriptor& attribute: layout.Attributes) {
            HashVertexAttribute(hash, attribute);
        }
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

void ValidateVertexInputRate(EVertexInputRate inputRate) {
    switch (inputRate) {
    case EVertexInputRate::VERTEX:
    case EVertexInputRate::INSTANCE:
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vertex input rate is invalid");
}

void ValidateTopology(EPrimitiveTopology topology) {
    switch (topology) {
    case EPrimitiveTopology::TRIANGLE_LIST:
    case EPrimitiveTopology::TRIANGLE_STRIP:
    case EPrimitiveTopology::LINE_LIST:
    case EPrimitiveTopology::LINE_STRIP:
    case EPrimitiveTopology::POINT_LIST:
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline topology is invalid");
}

void ValidatePolygonMode(EPolygonMode polygonMode) {
    switch (polygonMode) {
    case EPolygonMode::FILL:
    case EPolygonMode::LINE:
    case EPolygonMode::POINT:
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline polygon mode is invalid");
}

void ValidateCullMode(ECullMode cullMode) {
    switch (cullMode) {
    case ECullMode::NONE:
    case ECullMode::FRONT:
    case ECullMode::BACK:
    case ECullMode::FRONT_AND_BACK:
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline cull mode is invalid");
}

void ValidateFrontFace(EFrontFace frontFace) {
    switch (frontFace) {
    case EFrontFace::COUNTER_CLOCKWISE:
    case EFrontFace::CLOCKWISE:
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline front face is invalid");
}

void ValidateCompareOperation(ECompareOperation operation) {
    switch (operation) {
    case ECompareOperation::NEVER:
    case ECompareOperation::LESS:
    case ECompareOperation::EQUAL:
    case ECompareOperation::LESS_OR_EQUAL:
    case ECompareOperation::GREATER:
    case ECompareOperation::NOT_EQUAL:
    case ECompareOperation::GREATER_OR_EQUAL:
    case ECompareOperation::ALWAYS:
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline compare operation is invalid");
}

void ValidateBlendFactor(EBlendFactor factor) {
    switch (factor) {
    case EBlendFactor::ZERO:
    case EBlendFactor::ONE:
    case EBlendFactor::SOURCE_COLOR:
    case EBlendFactor::ONE_MINUS_SOURCE_COLOR:
    case EBlendFactor::DESTINATION_COLOR:
    case EBlendFactor::ONE_MINUS_DESTINATION_COLOR:
    case EBlendFactor::SOURCE_ALPHA:
    case EBlendFactor::ONE_MINUS_SOURCE_ALPHA:
    case EBlendFactor::DESTINATION_ALPHA:
    case EBlendFactor::ONE_MINUS_DESTINATION_ALPHA:
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline blend factor is invalid");
}

void ValidateBlendOperation(EBlendOperation operation) {
    switch (operation) {
    case EBlendOperation::ADD:
    case EBlendOperation::SUBTRACT:
    case EBlendOperation::REVERSE_SUBTRACT:
    case EBlendOperation::MINIMUM:
    case EBlendOperation::MAXIMUM:
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline blend operation is invalid");
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
    if (!shader.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Shader resource snapshot is invalid");
    }

    if (shader.GetEntryPoint().empty()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Shader entry point must not be empty");
    }

    if (!NResources::IsValidSpirVArtifact(shader.GetArtifact()->GetWords())) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Shader artifact is not valid SPIR-V");
    }
}

void ValidateVertexLayout(const VertexLayoutDescriptor& layout) {
    std::unordered_map<std::uint32_t, std::uint32_t> strides;

    for (const VertexBindingDescriptor& binding: layout.Bindings) {
        if (binding.StrideBytes == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vertex binding stride must be greater than zero");
        }

        ValidateVertexInputRate(binding.InputRate);

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
        ValidateBlendFactor(blend.SourceColorFactor);
        ValidateBlendFactor(blend.DestinationColorFactor);
        ValidateBlendOperation(blend.ColorOperation);
        ValidateBlendFactor(blend.SourceAlphaFactor);
        ValidateBlendFactor(blend.DestinationAlphaFactor);
        ValidateBlendOperation(blend.AlphaOperation);

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

bool operator==(const VertexLayoutDescriptor& left, const VertexLayoutDescriptor& right) noexcept {
    if (left.Bindings.size() != right.Bindings.size() || left.Attributes.size() != right.Attributes.size()) {
        return false;
    }

    const bool leftCanonical = IsCanonicalVertexLayout(left);
    const bool rightCanonical = IsCanonicalVertexLayout(right);

    if (leftCanonical != rightCanonical) {
        return false;
    }

    if (!leftCanonical) {
        return left.Bindings == right.Bindings && left.Attributes == right.Attributes;
    }

    return std::ranges::all_of(left.Bindings, [&](const VertexBindingDescriptor& binding) {
               const VertexBindingDescriptor* rightBinding = FindBinding(right.Bindings, binding.Binding);
               return rightBinding != nullptr && binding == *rightBinding;
           }) &&
           std::ranges::all_of(left.Attributes, [&](const VertexAttributeDescriptor& attribute) {
               const VertexAttributeDescriptor* rightAttribute = FindAttribute(right.Attributes, attribute.Location);
               return rightAttribute != nullptr && attribute == *rightAttribute;
           });
}

bool operator==(const GraphicsPipelineDescriptor& left, const GraphicsPipelineDescriptor& right) noexcept {
    const CanonicalShaderStages leftStages = GetCanonicalShaderStages(left.Shaders);
    const CanonicalShaderStages rightStages = GetCanonicalShaderStages(right.Shaders);

    if (leftStages.IsCanonical != rightStages.IsCanonical) {
        return false;
    }

    const bool shadersEqual = leftStages.IsCanonical
                                    ? EqualOptionalShader(leftStages.Vertex, rightStages.Vertex) &&
                                              EqualOptionalShader(leftStages.Fragment, rightStages.Fragment)
                                    : EqualShaderSequence(left.Shaders, right.Shaders);

    return shadersEqual && left.VertexLayout == right.VertexLayout && left.Topology == right.Topology &&
           left.RasterState == right.RasterState && left.DepthState == right.DepthState &&
           left.ColorAttachmentFormats == right.ColorAttachmentFormats &&
           left.ColorBlendAttachments == right.ColorBlendAttachments &&
           left.DepthAttachmentFormat == right.DepthAttachmentFormat && left.Samples == right.Samples;
}

void ValidateGraphicsPipelineDescriptor(const GraphicsPipelineDescriptor& descriptor) {
    bool hasVertexShader = false;
    bool hasFragmentShader = false;

    for (const Shader& shader: descriptor.Shaders) {
        ValidateShader(shader);

        switch (shader.GetArtifact()->GetStage()) {
        case NResources::EShaderStage::VERTEX:
            if (hasVertexShader) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                      "Graphics pipeline has duplicate vertex shader");
            }

            hasVertexShader = true;
            continue;
        case NResources::EShaderStage::FRAGMENT:
            if (hasFragmentShader) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                      "Graphics pipeline has duplicate fragment shader");
            }

            hasFragmentShader = true;
            continue;
        case NResources::EShaderStage::COMPUTE:
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline cannot contain compute shader");
        }

        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Shader stage is invalid");
    }

    if (!hasVertexShader) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Graphics pipeline requires a vertex shader");
    }

    if (!descriptor.ColorAttachmentFormats.empty() && !hasFragmentShader) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Graphics pipeline with color attachments requires a fragment shader");
    }

    ValidateVertexLayout(descriptor.VertexLayout);
    ValidateTopology(descriptor.Topology);
    ValidatePolygonMode(descriptor.RasterState.PolygonMode);
    ValidateCullMode(descriptor.RasterState.CullMode);
    ValidateFrontFace(descriptor.RasterState.FrontFace);
    ValidateCompareOperation(descriptor.DepthState.CompareOperation);
    ValidateAttachments(descriptor);
    ValidateSampleCount(descriptor.Samples);
}

std::uint64_t HashGraphicsPipelineDescriptor(const GraphicsPipelineDescriptor& descriptor) noexcept {
    std::uint64_t hash = HASH_OFFSET;
    const CanonicalShaderStages stages = GetCanonicalShaderStages(descriptor.Shaders);

    HashIntegral(hash, stages.IsCanonical);
    HashIntegral(hash, descriptor.Shaders.size());

    if (stages.IsCanonical) {
        HashOptionalShader(hash, stages.Vertex);
        HashOptionalShader(hash, stages.Fragment);
    } else {
        for (const Shader& shader: descriptor.Shaders) {
            HashShader(hash, shader);
        }
    }

    HashVertexLayout(hash, descriptor.VertexLayout);

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
