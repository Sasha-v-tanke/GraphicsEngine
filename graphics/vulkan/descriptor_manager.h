#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
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
    std::uint64_t ResourceVersion = 0;
    VkBufferUsageFlags Usage = 0;
    std::uint64_t BufferSizeBytes = 0;
    std::uint64_t OffsetBytes = 0;
    std::uint64_t SizeBytes = 0;
};

struct VulkanDescriptorImageBinding {
    VkImageView ImageView = VK_NULL_HANDLE;
    std::uint64_t ResourceVersion = 0;
    VkImageUsageFlags Usage = 0;
    VkImageLayout Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

struct VulkanDescriptorSamplerBinding {
    VkSampler Sampler = VK_NULL_HANDLE;
    std::uint64_t ResourceVersion = 0;
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

struct VulkanDescriptorLimits {
    std::uint64_t MinUniformBufferOffsetAlignment = 0;
    std::uint64_t MinStorageBufferOffsetAlignment = 0;
    std::uint64_t MaxUniformBufferRange = 0;
    std::uint64_t MaxStorageBufferRange = 0;
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
    explicit VulkanDescriptorManager(VkDevice device, VulkanDescriptorLimits limits);
    ~VulkanDescriptorManager();

    [[nodiscard]] VulkanDescriptorSetLease Acquire(const VulkanDescriptorRequest& request);
    void RetainUntil(VulkanDescriptorSetLease lease, std::uint64_t completionValue);
    void ReleaseCompleted(std::uint64_t completedValue) noexcept;

    [[nodiscard]] bool HasInFlightDescriptors() const noexcept {
        const std::scoped_lock lock{m_mutex};
        return !m_inFlightCompletions.empty();
    }

    [[nodiscard]] VulkanDescriptorTelemetry GetTelemetry() const;

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
            std::uint64_t FirstVersion = 0;
            std::uint64_t SecondVersion = 0;
            std::uint64_t FirstHandle = 0;
            std::uint64_t SecondHandle = 0;
            VkImageLayout ImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
    void ValidateBufferBinding(const VulkanResolvedMaterialBinding& binding) const;
    void ValidateBinding(const VulkanResolvedMaterialBinding& binding) const;
    void ValidateRequest(const VulkanDescriptorRequest& request) const;
    void WriteSet(VkDescriptorSet set, std::span<const VulkanResolvedMaterialBinding> bindings) const;

    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VulkanDescriptorLimits m_limits;
    VulkanDescriptorTelemetry m_telemetry;
    std::unordered_map<DescriptorKey, DescriptorEntry, DescriptorKeyHash> m_cache;
    std::vector<std::uint64_t> m_inFlightCompletions;
    mutable std::mutex m_mutex;
};

} // namespace NVulkan
