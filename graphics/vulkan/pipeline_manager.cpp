#include "pipeline_manager.h"

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

VulkanPipelineManager::VulkanPipelineManager(const VulkanDevice& device,
                                             const VulkanPhysicalDeviceSelection& physicalDevice)
    : m_device(device)
    , m_physicalDevice(physicalDevice) {
    const VkPipelineCacheCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .initialDataSize = 0,
            .pInitialData = nullptr,
    };
    const VkResult result = vkCreatePipelineCache(m_device.GetHandle(), &info, nullptr, &m_cache);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "vkCreatePipelineCache failed with VkResult {}",
                              static_cast<int>(result));
    }
}

VulkanPipelineManager::~VulkanPipelineManager() {
    m_pipelines.clear();
    vkDestroyPipelineCache(m_device.GetHandle(), m_cache, nullptr);
}

const VulkanGraphicsPipeline& VulkanPipelineManager::Prepare(const NGraphics::PipelineKey& key) {
    const std::lock_guard lock{m_mutex};
    const auto existing = m_pipelines.find(key);
    if (existing != m_pipelines.end()) {
        return *existing->second;
    }
    std::unique_ptr<VulkanGraphicsPipeline> pipeline =
            std::make_unique<VulkanGraphicsPipeline>(m_device, m_physicalDevice, key.GetDescriptor(), m_cache);
    const auto [entry, inserted] = m_pipelines.emplace(key, std::move(pipeline));
    (void)inserted;
    return *entry->second;
}

const VulkanGraphicsPipeline* VulkanPipelineManager::FindReady(const NGraphics::PipelineKey& key) const {
    const std::lock_guard lock{m_mutex};
    const auto entry = m_pipelines.find(key);
    return entry == m_pipelines.end() ? nullptr : entry->second.get();
}

std::size_t VulkanPipelineManager::GetPipelineCount() const {
    const std::lock_guard lock{m_mutex};
    return m_pipelines.size();
}

} // namespace NVulkan
