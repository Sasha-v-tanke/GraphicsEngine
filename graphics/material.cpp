#include "material.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>

namespace NGraphics {

namespace {

constexpr ShaderVisibilityFlags KNOWN_SHADER_VISIBILITY_MASK = ShaderVisibility(EShaderVisibility::Vertex) |
                                                               ShaderVisibility(EShaderVisibility::Fragment) |
                                                               ShaderVisibility(EShaderVisibility::Compute);
constexpr std::string_view BUFFER_RESOURCE_CLASS = "buffer";
constexpr std::string_view IMAGE_RESOURCE_CLASS = "image";
constexpr std::string_view PIPELINE_RESOURCE_CLASS = "pipeline";
constexpr std::string_view SAMPLER_RESOURCE_CLASS = "sampler";

[[nodiscard]] bool IsKnownBindingType(EMaterialBindingType type) noexcept {
    switch (type) {
    case EMaterialBindingType::UniformBuffer:
    case EMaterialBindingType::StorageBuffer:
    case EMaterialBindingType::SampledImage:
    case EMaterialBindingType::Sampler:
    case EMaterialBindingType::CombinedImageSampler:
        return true;
    }

    return false;
}

[[nodiscard]] bool IsResourceClass(const NResources::ResourceIdentity& identity,
                                   std::string_view resourceClass) noexcept {
    return identity.GetResourceClass() == resourceClass;
}

[[nodiscard]] const MaterialBindingLayoutEntry* FindLayoutEntry(std::span<const MaterialBindingLayoutEntry> layout,
                                                                std::uint32_t binding) noexcept {
    const auto it = std::ranges::find_if(layout, [binding](const MaterialBindingLayoutEntry& entry) {
        return entry.Binding == binding;
    });

    if (it == layout.end()) {
        return nullptr;
    }

    return &*it;
}

void ValidateLayout(std::span<const MaterialBindingLayoutEntry> layout) {
    for (std::size_t index = 0; index < layout.size(); ++index) {
        const MaterialBindingLayoutEntry& entry = layout[index];

        if (!IsKnownBindingType(entry.Type)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material binding layout contains unknown type");
        }

        if (entry.Visibility == 0 || (entry.Visibility & ~KNOWN_SHADER_VISIBILITY_MASK) != 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                  "Material binding layout contains invalid shader visibility");
        }

        if (entry.Count != 1) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material binding arrays are not supported yet");
        }

        for (std::size_t other = index + 1; other < layout.size(); ++other) {
            if (entry.Binding == layout[other].Binding) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                      "Material binding layout contains duplicate binding");
            }
        }
    }
}

void ValidateBindingValue(const MaterialBinding& binding) {
    switch (binding.Type) {
    case EMaterialBindingType::UniformBuffer:
    case EMaterialBindingType::StorageBuffer:
        if (!binding.Buffer.Resource.IsValid() || !IsResourceClass(binding.Buffer.Resource, BUFFER_RESOURCE_CLASS) ||
            binding.Buffer.SizeBytes == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material buffer binding is invalid");
        }

        if (binding.Buffer.OffsetBytes > std::numeric_limits<std::uint64_t>::max() - binding.Buffer.SizeBytes) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material buffer binding range is invalid");
        }
        return;
    case EMaterialBindingType::SampledImage:
        if (!binding.Image.Resource.IsValid() || !IsResourceClass(binding.Image.Resource, IMAGE_RESOURCE_CLASS)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material image binding is invalid");
        }
        return;
    case EMaterialBindingType::Sampler:
        if (!binding.Sampler.Resource.IsValid() || !IsResourceClass(binding.Sampler.Resource, SAMPLER_RESOURCE_CLASS)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material sampler binding is invalid");
        }
        return;
    case EMaterialBindingType::CombinedImageSampler:
        if (!binding.CombinedImageSampler.Image.IsValid() ||
            !IsResourceClass(binding.CombinedImageSampler.Image, IMAGE_RESOURCE_CLASS) ||
            !binding.CombinedImageSampler.Sampler.IsValid() ||
            !IsResourceClass(binding.CombinedImageSampler.Sampler, SAMPLER_RESOURCE_CLASS)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                                  "Material combined image sampler binding is invalid");
        }
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material binding contains unknown type");
}

void ValidateBindings(std::span<const MaterialBindingLayoutEntry> layout, std::span<const MaterialBinding> bindings) {
    if (bindings.size() != layout.size()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material bindings must match layout binding count");
    }

    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const MaterialBinding& binding = bindings[index];
        const MaterialBindingLayoutEntry* layoutEntry = FindLayoutEntry(layout, binding.Binding);

        if (layoutEntry == nullptr) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material binding is not declared in layout");
        }

        if (binding.Type != layoutEntry->Type) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material binding type does not match layout");
        }

        for (std::size_t other = index + 1; other < bindings.size(); ++other) {
            if (binding.Binding == bindings[other].Binding) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material contains duplicate binding");
            }
        }

        ValidateBindingValue(binding);
    }
}

} // namespace

Material::Material(NResources::ResourceIdentity pipeline,
                   std::vector<MaterialBindingLayoutEntry> layout,
                   std::vector<MaterialBinding> bindings)
    : m_pipeline(std::move(pipeline))
    , m_layout(std::move(layout))
    , m_bindings(std::move(bindings)) {
    if (!m_pipeline.IsValid() || !IsResourceClass(m_pipeline, PIPELINE_RESOURCE_CLASS)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Material pipeline identity is invalid");
    }

    ValidateLayout(m_layout);
    ValidateBindings(m_layout, m_bindings);
}

} // namespace NGraphics
