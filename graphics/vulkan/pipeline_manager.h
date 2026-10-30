#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

#include <GraphicsEngine/graphics/pipeline_key.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <graphics/vulkan/graphics_pipeline.h>

namespace NVulkan {

// Backend-owned cache for one device. Device/selection must match and outlive the
// manager. Entries never evict: all returned pointers remain valid until destruction.
// Stop preparation/recording and complete every GPU use before destroying the manager,
// then destroy the device. No process-global cache or cross-device native identities.
class VulkanPipelineManager final: public NCommon::NonTransferable {
public:
    VulkanPipelineManager(const VulkanDevice& device, const VulkanPhysicalDeviceSelection& physicalDevice);
    ~VulkanPipelineManager();

    // Explicit synchronous preparation/warmup, never called implicitly by FindReady.
    // May run on a caller-owned background executor. Concurrent preparation serializes
    // driver cache access and publishes exactly one pipeline per key. Failure leaves
    // no entry; a subsequent preparation can retry. No internal tasks outlive this call.
    [[nodiscard]] const VulkanGraphicsPipeline& Prepare(const NGraphics::PipelineKey& key);

    // Lookup only; missing => nullptr (caller chooses skip/fallback). Does not create
    // Vulkan objects. May briefly contend with preparation; baseline is pre-frame warmup.
    [[nodiscard]] const VulkanGraphicsPipeline* FindReady(const NGraphics::PipelineKey& key) const;
    [[nodiscard]] std::size_t GetPipelineCount() const;

private:
    const VulkanDevice& m_device;
    VulkanPhysicalDeviceSelection m_physicalDevice;
    VkPipelineCache m_cache = VK_NULL_HANDLE;
    mutable std::mutex m_mutex;
    std::unordered_map<NGraphics::PipelineKey, std::unique_ptr<VulkanGraphicsPipeline>, NGraphics::PipelineKeyHash>
            m_pipelines;
};

} // namespace NVulkan
