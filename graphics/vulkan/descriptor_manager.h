#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>
#include <vulkan.h>

#include <GraphicsEngine/graphics/material.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NVulkan {

struct VulkanDescriptorTelemetry {
    std::uint64_t Allocations = 0;
    std::uint64_t CacheHits = 0;
    std::uint64_t CacheMisses = 0;
};

struct VulkanDescriptorBufferBinding {
    VkBuffer Buffer = VK_NULL_HANDLE;
    std::uint64_t OffsetBytes = 0;
    std::uint64_t SizeBytes = 0;
};

struct VulkanDescriptorImageBinding {
    VkImageView ImageView = VK_NULL_HANDLE;
    VkImageLayout Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

struct VulkanDescriptorSamplerBinding {
    VkSampler Sampler = VK_NULL_HANDLE;
};

struct VulkanResolvedMaterialBinding {
    NGraphics::MaterialBinding Material;
    VulkanDescriptorBufferBinding Buffer;
    VulkanDescriptorImageBinding Image;
    VulkanDescriptorSamplerBinding Sampler;
};

struct VulkanDescriptorRequest {
    std::span<const NGraphics::MaterialBindingLayoutEntry> Layout;
    std::span<const VulkanResolvedMaterialBinding> Bindings;
};

class VulkanDescriptorSetLease final {
public:
    VulkanDescriptorSetLease() = default;

    [[nodiscard]] VkDescriptorSetLayout GetLayout() const noexcept {
        return m_layout;
    }

    [[nodiscard]] VkDescriptorSet GetSet() const noexcept {
        return m_set;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return m_layout != VK_NULL_HANDLE && m_set != VK_NULL_HANDLE;
    }

private:
    VulkanDescriptorSetLease(VkDescriptorSetLayout layout, VkDescriptorSet set) noexcept
        : m_layout(layout)
        , m_set(set) {
    }

    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;

    friend class VulkanDescriptorManager;
};

class VulkanDescriptorManager final: public NCommon::NonTransferable {
public:
    explicit VulkanDescriptorManager(VkDevice device);
    ~VulkanDescriptorManager();

    [[nodiscard]] VulkanDescriptorSetLease Acquire(const VulkanDescriptorRequest& request);

    [[nodiscard]] const VulkanDescriptorTelemetry& GetTelemetry() const noexcept {
        return m_telemetry;
    }

private:
    struct DescriptorKey {
        struct LayoutEntry {
            std::uint32_t Binding = 0;
            NGraphics::EMaterialBindingType Type = NGraphics::EMaterialBindingType::UniformBuffer;
            NGraphics::ShaderVisibilityFlags Visibility = 0;
            std::uint32_t Count = 0;

            [[nodiscard]] friend bool operator==(const LayoutEntry&, const LayoutEntry&) noexcept = default;
        };

        struct BindingEntry {
            std::uint32_t Binding = 0;
            NGraphics::EMaterialBindingType Type = NGraphics::EMaterialBindingType::UniformBuffer;
            NResources::ResourceIdentity First;
            NResources::ResourceIdentity Second;
            std::uint64_t OffsetBytes = 0;
            std::uint64_t SizeBytes = 0;

            [[nodiscard]] friend bool operator==(const BindingEntry&, const BindingEntry&) noexcept = default;
        };

        std::vector<LayoutEntry> Layout;
        std::vector<BindingEntry> Bindings;

        [[nodiscard]] friend bool operator==(const DescriptorKey&, const DescriptorKey&) noexcept = default;
    };

    struct DescriptorKeyHash {
        [[nodiscard]] std::size_t operator()(const DescriptorKey& key) const noexcept;
    };

    struct DescriptorEntry {
        VkDescriptorSetLayout Layout = VK_NULL_HANDLE;
        VkDescriptorSet Set = VK_NULL_HANDLE;
    };

    [[nodiscard]] static DescriptorKey MakeKey(const VulkanDescriptorRequest& request);
    [[nodiscard]] VkDescriptorSetLayout
    GetOrCreateLayout(std::span<const NGraphics::MaterialBindingLayoutEntry> layout);
    [[nodiscard]] VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout,
                                              std::span<const NGraphics::MaterialBindingLayoutEntry> entries);
    void WriteSet(VkDescriptorSet set, std::span<const VulkanResolvedMaterialBinding> bindings) const;

    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VulkanDescriptorTelemetry m_telemetry;
    std::unordered_map<DescriptorKey, DescriptorEntry, DescriptorKeyHash> m_cache;
};

} // namespace NVulkan
