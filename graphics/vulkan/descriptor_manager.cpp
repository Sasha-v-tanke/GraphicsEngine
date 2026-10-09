#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <string>
#include <utility>

#include <graphics/vulkan/descriptor_manager.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

namespace {

constexpr std::uint32_t DESCRIPTOR_POOL_SET_COUNT = 4096;
constexpr std::uint32_t DESCRIPTOR_POOL_BINDING_COUNT = 4096;

template<class T>
void HashCombine(std::size_t& seed, const T& value) noexcept {
    seed ^= std::hash<T>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}

void HashIdentity(std::size_t& seed, const NResources::ResourceIdentity& identity) noexcept {
    HashCombine(seed, std::string{identity.GetResourceClass()});
    HashCombine(seed, std::string{identity.GetKey()});
}

[[nodiscard]] VkDescriptorType ToDescriptorType(NGraphics::EMaterialBindingType type) {
    switch (type) {
    case NGraphics::EMaterialBindingType::UniformBuffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case NGraphics::EMaterialBindingType::StorageBuffer:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case NGraphics::EMaterialBindingType::SampledImage:
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case NGraphics::EMaterialBindingType::Sampler:
        return VK_DESCRIPTOR_TYPE_SAMPLER;
    case NGraphics::EMaterialBindingType::CombinedImageSampler:
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unsupported material binding type");
}

[[nodiscard]] VkShaderStageFlags ToShaderStages(NGraphics::ShaderVisibilityFlags visibility) {
    VkShaderStageFlags stages = 0;

    if ((visibility & NGraphics::ShaderVisibility(NGraphics::EShaderVisibility::Vertex)) != 0) {
        stages |= VK_SHADER_STAGE_VERTEX_BIT;
    }

    if ((visibility & NGraphics::ShaderVisibility(NGraphics::EShaderVisibility::Fragment)) != 0) {
        stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    if ((visibility & NGraphics::ShaderVisibility(NGraphics::EShaderVisibility::Compute)) != 0) {
        stages |= VK_SHADER_STAGE_COMPUTE_BIT;
    }

    return stages;
}

void ValidateLayout(std::span<const NGraphics::MaterialBindingLayoutEntry> layout) {
    if (layout.empty()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Descriptor layout is empty");
    }

    for (const NGraphics::MaterialBindingLayoutEntry& entry: layout) {
        if (entry.Visibility == 0 || entry.Count == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Descriptor layout entry is invalid");
        }
        (void)ToDescriptorType(entry.Type);
    }
}

} // namespace

std::size_t VulkanDescriptorManager::DescriptorKeyHash::operator()(const DescriptorKey& key) const noexcept {
    std::size_t seed = 0;

    for (const DescriptorKey::LayoutEntry& entry: key.Layout) {
        HashCombine(seed, entry.Binding);
        HashCombine(seed, static_cast<std::uint32_t>(entry.Type));
        HashCombine(seed, entry.Visibility);
        HashCombine(seed, entry.Count);
    }

    for (const DescriptorKey::BindingEntry& entry: key.Bindings) {
        HashCombine(seed, entry.Binding);
        HashCombine(seed, static_cast<std::uint32_t>(entry.Type));
        HashIdentity(seed, entry.First);
        HashIdentity(seed, entry.Second);
        HashCombine(seed, entry.FirstVersion);
        HashCombine(seed, entry.SecondVersion);
        HashCombine(seed, entry.FirstHandle);
        HashCombine(seed, entry.SecondHandle);
        HashCombine(seed, static_cast<std::uint32_t>(entry.ImageLayout));
        HashCombine(seed, entry.OffsetBytes);
        HashCombine(seed, entry.SizeBytes);
    }

    return seed;
}

VulkanDescriptorManager::DescriptorKey VulkanDescriptorManager::MakeKey(const VulkanDescriptorRequest& request) {
    VulkanDescriptorManager::DescriptorKey key;
    key.Layout.reserve(request.Layout.size());
    key.Bindings.reserve(request.Bindings.size());

    for (const NGraphics::MaterialBindingLayoutEntry& entry: request.Layout) {
        key.Layout.push_back({
                .Binding = entry.Binding,
                .Type = entry.Type,
                .Visibility = entry.Visibility,
                .Count = entry.Count,
        });
    }

    for (const VulkanResolvedMaterialBinding& binding: request.Bindings) {
        VulkanDescriptorManager::DescriptorKey::BindingEntry entry{
                .Binding = binding.Material.Binding,
                .Type = binding.Material.Type,
        };

        switch (binding.Material.Type) {
        case NGraphics::EMaterialBindingType::UniformBuffer:
        case NGraphics::EMaterialBindingType::StorageBuffer:
            entry.First = binding.Material.Buffer.Resource;
            entry.FirstVersion = binding.Buffer.ResourceVersion;
            entry.FirstHandle = reinterpret_cast<std::uint64_t>(binding.Buffer.Buffer);
            entry.OffsetBytes = binding.Material.Buffer.OffsetBytes;
            entry.SizeBytes = binding.Material.Buffer.SizeBytes;
            break;
        case NGraphics::EMaterialBindingType::SampledImage:
            entry.First = binding.Material.Image.Resource;
            entry.FirstVersion = binding.Image.ResourceVersion;
            entry.FirstHandle = reinterpret_cast<std::uint64_t>(binding.Image.ImageView);
            entry.ImageLayout = binding.Image.Layout;
            break;
        case NGraphics::EMaterialBindingType::Sampler:
            entry.First = binding.Material.Sampler.Resource;
            entry.FirstVersion = binding.Sampler.ResourceVersion;
            entry.FirstHandle = reinterpret_cast<std::uint64_t>(binding.Sampler.Sampler);
            break;
        case NGraphics::EMaterialBindingType::CombinedImageSampler:
            entry.First = binding.Material.CombinedImageSampler.Image;
            entry.Second = binding.Material.CombinedImageSampler.Sampler;
            entry.FirstVersion = binding.Image.ResourceVersion;
            entry.SecondVersion = binding.Sampler.ResourceVersion;
            entry.FirstHandle = reinterpret_cast<std::uint64_t>(binding.Image.ImageView);
            entry.SecondHandle = reinterpret_cast<std::uint64_t>(binding.Sampler.Sampler);
            entry.ImageLayout = binding.Image.Layout;
            break;
        }

        key.Bindings.push_back(std::move(entry));
    }

    std::ranges::sort(key.Layout, {}, &VulkanDescriptorManager::DescriptorKey::LayoutEntry::Binding);
    std::ranges::sort(key.Bindings, {}, &VulkanDescriptorManager::DescriptorKey::BindingEntry::Binding);

    return key;
}

VulkanDescriptorManager::VulkanDescriptorManager(VkDevice device, VulkanDescriptorLimits limits)
    : m_device(device)
    , m_limits(limits) {
    if (m_device == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan descriptor manager device is null");
    }

    const std::array poolSizes{
            VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                 .descriptorCount = DESCRIPTOR_POOL_BINDING_COUNT},
            VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                 .descriptorCount = DESCRIPTOR_POOL_BINDING_COUNT},
            VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                 .descriptorCount = DESCRIPTOR_POOL_BINDING_COUNT},
            VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = DESCRIPTOR_POOL_BINDING_COUNT},
            VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 .descriptorCount = DESCRIPTOR_POOL_BINDING_COUNT},
    };

    const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .maxSets = DESCRIPTOR_POOL_SET_COUNT,
            .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
            .pPoolSizes = poolSizes.data(),
    };

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::OUT_OF_MEMORY, "Failed to create Vulkan descriptor pool");
    }
}

VulkanDescriptorManager::~VulkanDescriptorManager() {
    if (HasInFlightDescriptors()) {
        std::terminate();
    }

    for (const auto& [_, entry]: m_cache) {
        if (entry.Layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, entry.Layout, nullptr);
        }
    }

    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    }
}

VulkanDescriptorSetLease VulkanDescriptorManager::Acquire(const VulkanDescriptorRequest& request) {
    ValidateLayout(request.Layout);

    if (request.Layout.size() != request.Bindings.size()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Descriptor bindings must match layout");
    }

    for (const VulkanResolvedMaterialBinding& binding: request.Bindings) {
        ValidateBinding(binding);
    }

    DescriptorKey key = MakeKey(request);

    if (const auto it = m_cache.find(key); it != m_cache.end()) {
        ++m_telemetry.CacheHits;
        return VulkanDescriptorSetLease{it->second.Layout, it->second.Set};
    }

    ++m_telemetry.CacheMisses;
    const VkDescriptorSetLayout layout = GetOrCreateLayout(request.Layout);
    const VkDescriptorSet set = AllocateSet(layout, request.Layout);
    WriteSet(set, request.Bindings);

    m_cache.emplace(std::move(key), DescriptorEntry{.Layout = layout, .Set = set});
    ++m_telemetry.Allocations;

    return VulkanDescriptorSetLease{layout, set};
}

void VulkanDescriptorManager::RetainUntil(VulkanDescriptorSetLease lease, std::uint64_t completionValue) {
    if (!lease.IsValid() || completionValue == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Descriptor retention request is invalid");
    }

    m_inFlightCompletions.push_back(completionValue);
}

void VulkanDescriptorManager::ReleaseCompleted(std::uint64_t completedValue) noexcept {
    std::erase_if(m_inFlightCompletions,
                  [completedValue](std::uint64_t completion) { return completion <= completedValue; });
}

VkDescriptorSetLayout
VulkanDescriptorManager::GetOrCreateLayout(std::span<const NGraphics::MaterialBindingLayoutEntry> layout) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(layout.size());

    for (const NGraphics::MaterialBindingLayoutEntry& entry: layout) {
        bindings.push_back({
                .binding = entry.Binding,
                .descriptorType = ToDescriptorType(entry.Type),
                .descriptorCount = entry.Count,
                .stageFlags = ToShaderStages(entry.Visibility),
                .pImmutableSamplers = nullptr,
        });
    }

    const VkDescriptorSetLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .bindingCount = static_cast<std::uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
    };

    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &descriptorLayout) != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::OUT_OF_MEMORY, "Failed to create Vulkan descriptor set layout");
    }

    return descriptorLayout;
}

VkDescriptorSet
VulkanDescriptorManager::AllocateSet(VkDescriptorSetLayout layout,
                                     [[maybe_unused]] std::span<const NGraphics::MaterialBindingLayoutEntry> entries) {
    const VkDescriptorSetAllocateInfo allocateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = nullptr,
            .descriptorPool = m_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &layout,
    };

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_device, &allocateInfo, &set) != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::OUT_OF_MEMORY, "Failed to allocate Vulkan descriptor set");
    }

    return set;
}

void VulkanDescriptorManager::ValidateBufferBinding(const VulkanResolvedMaterialBinding& binding) const {
    if (binding.Buffer.Buffer == VK_NULL_HANDLE || binding.Buffer.ResourceVersion == 0 ||
        binding.Buffer.BufferSizeBytes == 0 || binding.Buffer.SizeBytes == 0 ||
        binding.Buffer.OffsetBytes > std::numeric_limits<std::uint64_t>::max() - binding.Buffer.SizeBytes ||
        binding.Buffer.OffsetBytes + binding.Buffer.SizeBytes > binding.Buffer.BufferSizeBytes ||
        binding.Material.Buffer.OffsetBytes != binding.Buffer.OffsetBytes ||
        binding.Material.Buffer.SizeBytes != binding.Buffer.SizeBytes) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Descriptor buffer binding is invalid");
    }

    const bool isUniform = binding.Material.Type == NGraphics::EMaterialBindingType::UniformBuffer;
    const std::uint64_t alignment =
            isUniform ? m_limits.MinUniformBufferOffsetAlignment : m_limits.MinStorageBufferOffsetAlignment;
    const std::uint64_t maxRange = isUniform ? m_limits.MaxUniformBufferRange : m_limits.MaxStorageBufferRange;

    if (alignment == 0 || (binding.Buffer.OffsetBytes % alignment) != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Descriptor buffer offset alignment is invalid");
    }

    if (maxRange != 0 && binding.Buffer.SizeBytes > maxRange) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Descriptor buffer range exceeds device limits");
    }
}

void VulkanDescriptorManager::ValidateBinding(const VulkanResolvedMaterialBinding& binding) const {
    switch (binding.Material.Type) {
    case NGraphics::EMaterialBindingType::UniformBuffer:
    case NGraphics::EMaterialBindingType::StorageBuffer:
        ValidateBufferBinding(binding);
        return;
    case NGraphics::EMaterialBindingType::SampledImage:
        if (binding.Image.ImageView == VK_NULL_HANDLE || binding.Image.ResourceVersion == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Descriptor image binding is invalid");
        }
        return;
    case NGraphics::EMaterialBindingType::Sampler:
        if (binding.Sampler.Sampler == VK_NULL_HANDLE || binding.Sampler.ResourceVersion == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Descriptor sampler binding is invalid");
        }
        return;
    case NGraphics::EMaterialBindingType::CombinedImageSampler:
        if (binding.Image.ImageView == VK_NULL_HANDLE || binding.Image.ResourceVersion == 0 ||
            binding.Sampler.Sampler == VK_NULL_HANDLE || binding.Sampler.ResourceVersion == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Descriptor combined image sampler is invalid");
        }
        return;
    }

    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unsupported descriptor binding type");
}

void VulkanDescriptorManager::WriteSet(VkDescriptorSet set,
                                       std::span<const VulkanResolvedMaterialBinding> bindings) const {
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkDescriptorImageInfo> imageInfos;
    std::vector<VkWriteDescriptorSet> writes;
    bufferInfos.reserve(bindings.size());
    imageInfos.reserve(bindings.size());
    writes.reserve(bindings.size());

    for (const VulkanResolvedMaterialBinding& binding: bindings) {
        VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = set,
                .dstBinding = binding.Material.Binding,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = ToDescriptorType(binding.Material.Type),
                .pImageInfo = nullptr,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
        };

        switch (binding.Material.Type) {
        case NGraphics::EMaterialBindingType::UniformBuffer:
        case NGraphics::EMaterialBindingType::StorageBuffer:
            bufferInfos.push_back({
                    .buffer = binding.Buffer.Buffer,
                    .offset = binding.Buffer.OffsetBytes,
                    .range = binding.Buffer.SizeBytes,
            });
            write.pBufferInfo = &bufferInfos.back();
            break;
        case NGraphics::EMaterialBindingType::SampledImage:
            imageInfos.push_back({
                    .sampler = VK_NULL_HANDLE,
                    .imageView = binding.Image.ImageView,
                    .imageLayout = binding.Image.Layout,
            });
            write.pImageInfo = &imageInfos.back();
            break;
        case NGraphics::EMaterialBindingType::Sampler:
            imageInfos.push_back({
                    .sampler = binding.Sampler.Sampler,
                    .imageView = VK_NULL_HANDLE,
                    .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            });
            write.pImageInfo = &imageInfos.back();
            break;
        case NGraphics::EMaterialBindingType::CombinedImageSampler:
            imageInfos.push_back({
                    .sampler = binding.Sampler.Sampler,
                    .imageView = binding.Image.ImageView,
                    .imageLayout = binding.Image.Layout,
            });
            write.pImageInfo = &imageInfos.back();
            break;
        }

        writes.push_back(write);
    }

    vkUpdateDescriptorSets(m_device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

} // namespace NVulkan
