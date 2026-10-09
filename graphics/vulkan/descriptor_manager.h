#pragma once

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
    struct DescriptorKey;
    struct DescriptorKeyHash;
    struct DescriptorEntry;

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
