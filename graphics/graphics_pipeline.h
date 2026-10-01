#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <GraphicsEngine/graphics/shader.h>

namespace NGraphics {

enum class EVertexFormat {
    FLOAT32,
    FLOAT32_2,
    FLOAT32_3,
    FLOAT32_4,
    UINT32,
    UINT32_2,
    UINT32_3,
    UINT32_4,
};

enum class EVertexInputRate {
    VERTEX,
    INSTANCE,
};

struct VertexBindingDescriptor {
    std::uint32_t Binding = 0;
    std::uint32_t StrideBytes = 0;
    EVertexInputRate InputRate = EVertexInputRate::VERTEX;

    [[nodiscard]] friend bool operator==(const VertexBindingDescriptor& left,
                                         const VertexBindingDescriptor& right) noexcept = default;
};

struct VertexAttributeDescriptor {
    std::uint32_t Location = 0;
    std::uint32_t Binding = 0;
    EVertexFormat Format = EVertexFormat::FLOAT32;
    std::uint32_t OffsetBytes = 0;

    [[nodiscard]] friend bool operator==(const VertexAttributeDescriptor& left,
                                         const VertexAttributeDescriptor& right) noexcept = default;
};

struct VertexLayoutDescriptor {
    std::vector<VertexBindingDescriptor> Bindings;
    std::vector<VertexAttributeDescriptor> Attributes;

    [[nodiscard]] friend bool operator==(const VertexLayoutDescriptor& left,
                                         const VertexLayoutDescriptor& right) noexcept = default;
};

enum class EPrimitiveTopology {
    TRIANGLE_LIST,
    TRIANGLE_STRIP,
    LINE_LIST,
    LINE_STRIP,
    POINT_LIST,
};

enum class EPolygonMode {
    FILL,
    LINE,
    POINT,
};

enum class ECullMode {
    NONE,
    FRONT,
    BACK,
    FRONT_AND_BACK,
};

enum class EFrontFace {
    COUNTER_CLOCKWISE,
    CLOCKWISE,
};

struct RasterStateDescriptor {
    EPolygonMode PolygonMode = EPolygonMode::FILL;
    ECullMode CullMode = ECullMode::BACK;
    EFrontFace FrontFace = EFrontFace::COUNTER_CLOCKWISE;

    [[nodiscard]] friend bool operator==(const RasterStateDescriptor& left,
                                         const RasterStateDescriptor& right) noexcept = default;
};

enum class ECompareOperation {
    NEVER,
    LESS,
    EQUAL,
    LESS_OR_EQUAL,
    GREATER,
    NOT_EQUAL,
    GREATER_OR_EQUAL,
    ALWAYS,
};

struct DepthStateDescriptor {
    bool TestEnabled = false;
    bool WriteEnabled = false;
    ECompareOperation CompareOperation = ECompareOperation::LESS;

    [[nodiscard]] friend bool operator==(const DepthStateDescriptor& left,
                                         const DepthStateDescriptor& right) noexcept = default;
};

enum class EBlendFactor {
    ZERO,
    ONE,
    SOURCE_COLOR,
    ONE_MINUS_SOURCE_COLOR,
    DESTINATION_COLOR,
    ONE_MINUS_DESTINATION_COLOR,
    SOURCE_ALPHA,
    ONE_MINUS_SOURCE_ALPHA,
    DESTINATION_ALPHA,
    ONE_MINUS_DESTINATION_ALPHA,
};

enum class EBlendOperation {
    ADD,
    SUBTRACT,
    REVERSE_SUBTRACT,
    MINIMUM,
    MAXIMUM,
};

enum class EColorComponent : std::uint32_t {
    RED = 1U << 0U,
    GREEN = 1U << 1U,
    BLUE = 1U << 2U,
    ALPHA = 1U << 3U,
};

using ColorComponentFlags = std::uint32_t;

[[nodiscard]] constexpr ColorComponentFlags ColorComponent(EColorComponent component) noexcept {
    return static_cast<ColorComponentFlags>(component);
}

[[nodiscard]] constexpr ColorComponentFlags operator|(EColorComponent left, EColorComponent right) noexcept {
    return ColorComponent(left) | ColorComponent(right);
}

[[nodiscard]] constexpr ColorComponentFlags operator|(ColorComponentFlags left, EColorComponent right) noexcept {
    return left | ColorComponent(right);
}

inline constexpr ColorComponentFlags ALL_COLOR_COMPONENTS =
        EColorComponent::RED | EColorComponent::GREEN | EColorComponent::BLUE | EColorComponent::ALPHA;

struct BlendAttachmentDescriptor {
    bool Enabled = false;
    EBlendFactor SourceColorFactor = EBlendFactor::ONE;
    EBlendFactor DestinationColorFactor = EBlendFactor::ZERO;
    EBlendOperation ColorOperation = EBlendOperation::ADD;
    EBlendFactor SourceAlphaFactor = EBlendFactor::ONE;
    EBlendFactor DestinationAlphaFactor = EBlendFactor::ZERO;
    EBlendOperation AlphaOperation = EBlendOperation::ADD;
    ColorComponentFlags WriteMask = ALL_COLOR_COMPONENTS;

    [[nodiscard]] friend bool operator==(const BlendAttachmentDescriptor& left,
                                         const BlendAttachmentDescriptor& right) noexcept = default;
};

enum class EPixelFormat {
    UNDEFINED,
    RGBA8_UNORM,
    BGRA8_UNORM,
    RGBA8_SRGB,
    BGRA8_SRGB,
    RGBA16_FLOAT,
    D32_FLOAT,
    D24_UNORM_S8_UINT,
};

enum class ESampleCount : std::uint32_t {
    X1 = 1,
    X2 = 2,
    X4 = 4,
    X8 = 8,
};

struct GraphicsPipelineDescriptor {
    std::vector<Shader> Shaders;
    VertexLayoutDescriptor VertexLayout;
    EPrimitiveTopology Topology = EPrimitiveTopology::TRIANGLE_LIST;
    RasterStateDescriptor RasterState;
    DepthStateDescriptor DepthState;
    std::vector<EPixelFormat> ColorAttachmentFormats;
    std::vector<BlendAttachmentDescriptor> ColorBlendAttachments;
    EPixelFormat DepthAttachmentFormat = EPixelFormat::UNDEFINED;
    ESampleCount Samples = ESampleCount::X1;

    [[nodiscard]] friend bool operator==(const GraphicsPipelineDescriptor& left,
                                         const GraphicsPipelineDescriptor& right) noexcept = default;
};

class GraphicsPipelineHandle {
public:
    GraphicsPipelineHandle() = default;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_ownerId != 0 && m_value != 0 && m_generation != 0;
    }

    [[nodiscard]] std::uint64_t GetValue() const noexcept {
        return m_value;
    }

    [[nodiscard]] std::uint64_t GetGeneration() const noexcept {
        return m_generation;
    }

    [[nodiscard]] friend bool operator==(GraphicsPipelineHandle left, GraphicsPipelineHandle right) noexcept = default;

private:
    GraphicsPipelineHandle(std::uint64_t ownerId, std::uint64_t value, std::uint64_t generation) noexcept
        : m_ownerId(ownerId)
        , m_value(value)
        , m_generation(generation) {
    }

    std::uint64_t m_ownerId = 0;
    std::uint64_t m_value = 0;
    std::uint64_t m_generation = 0;

    friend class Graphics;
};

void ValidateGraphicsPipelineDescriptor(const GraphicsPipelineDescriptor& descriptor);

[[nodiscard]] std::uint64_t HashGraphicsPipelineDescriptor(const GraphicsPipelineDescriptor& descriptor) noexcept;

} // namespace NGraphics
