#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <GraphicsEngine/graphics/buffer.h>
#include <GraphicsEngine/graphics/image.h>
#include <GraphicsEngine/resources/resource_identity.h>

namespace NGraphics {

enum class EMaterialBindingType {
    UniformBuffer,
    StorageBuffer,
    SampledImage,
    Sampler,
    CombinedImageSampler,
};

enum class EShaderVisibility : std::uint32_t {
    Vertex = 1U << 0U,
    Fragment = 1U << 1U,
    Compute = 1U << 2U,
};

using ShaderVisibilityFlags = std::uint32_t;

[[nodiscard]] constexpr ShaderVisibilityFlags ShaderVisibility(EShaderVisibility visibility) noexcept {
    return static_cast<ShaderVisibilityFlags>(visibility);
}

[[nodiscard]] constexpr ShaderVisibilityFlags operator|(EShaderVisibility left, EShaderVisibility right) noexcept {
    return ShaderVisibility(left) | ShaderVisibility(right);
}

[[nodiscard]] constexpr ShaderVisibilityFlags operator|(ShaderVisibilityFlags left, EShaderVisibility right) noexcept {
    return left | ShaderVisibility(right);
}

struct MaterialBindingLayoutEntry {
    std::uint32_t Binding = 0;
    EMaterialBindingType Type = EMaterialBindingType::UniformBuffer;
    ShaderVisibilityFlags Visibility = 0;
    std::uint32_t Count = 1;
};

struct MaterialBufferBinding {
    BufferHandle Buffer;
    std::uint64_t OffsetBytes = 0;
    std::uint64_t SizeBytes = 0;
};

struct MaterialImageBinding {
    ImageViewHandle ImageView;
};

struct MaterialSamplerBinding {
    SamplerHandle Sampler;
};

struct MaterialCombinedImageSamplerBinding {
    ImageViewHandle ImageView;
    SamplerHandle Sampler;
};

struct MaterialBinding {
    std::uint32_t Binding = 0;
    EMaterialBindingType Type = EMaterialBindingType::UniformBuffer;
    MaterialBufferBinding Buffer;
    MaterialImageBinding Image;
    MaterialSamplerBinding Sampler;
    MaterialCombinedImageSamplerBinding CombinedImageSampler;
};

class Material final {
public:
    explicit Material(NResources::ResourceIdentity pipeline,
                      std::vector<MaterialBindingLayoutEntry> layout = {},
                      std::vector<MaterialBinding> bindings = {});

    [[nodiscard]] const NResources::ResourceIdentity& GetPipeline() const noexcept {
        return m_pipeline;
    }

    [[nodiscard]] std::span<const MaterialBindingLayoutEntry> GetLayout() const noexcept {
        return m_layout;
    }

    [[nodiscard]] std::span<const MaterialBinding> GetBindings() const noexcept {
        return m_bindings;
    }

private:
    NResources::ResourceIdentity m_pipeline;
    std::vector<MaterialBindingLayoutEntry> m_layout;
    std::vector<MaterialBinding> m_bindings;
};

} // namespace NGraphics
