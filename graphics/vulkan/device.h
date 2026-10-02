#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <vulkan.h>

#include <GraphicsEngine/graphics/graphics_capabilities.h>
#include <GraphicsEngine/graphics/vulkan/physical_device.h>
#include <GraphicsEngine/lib/common/wrapper/non_copyable.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NVulkan {

struct VulkanDeviceQueuePlan {
    std::uint32_t FamilyIndex = 0;
};

struct VulkanDevicePlan {
    std::vector<VulkanDeviceQueuePlan> QueueFamilies;
    std::vector<std::string> Extensions;
    VulkanPhysicalDeviceFeatures Features;
    NGraphics::GraphicsCapabilities GraphicsCapabilities;
};

class VulkanLockedQueue final: public NCommon::NonCopyable {
public:
    VulkanLockedQueue(std::unique_lock<std::mutex> lock, VkQueue handle, std::uint32_t familyIndex) noexcept;

    VulkanLockedQueue(VulkanLockedQueue&&) noexcept = default;
    VulkanLockedQueue& operator=(VulkanLockedQueue&&) noexcept = default;

    [[nodiscard]] VkQueue GetHandle() const noexcept;

    [[nodiscard]] std::uint32_t GetFamilyIndex() const noexcept;

private:
    std::unique_lock<std::mutex> m_lock;
    VkQueue m_handle = VK_NULL_HANDLE;
    std::uint32_t m_familyIndex = 0;
};

[[nodiscard]] VulkanDevicePlan MakeVulkanDevicePlan(const VulkanPhysicalDeviceSelection& physicalDevice);

class VulkanDevice final: public NCommon::NonTransferable {
public:
    explicit VulkanDevice(const VulkanPhysicalDeviceSelection& physicalDevice);

    ~VulkanDevice();

    [[nodiscard]] VkDevice GetHandle() const noexcept;

    [[nodiscard]] const NGraphics::GraphicsCapabilities& GetGraphicsCapabilities() const noexcept;

    [[nodiscard]] VulkanLockedQueue LockGraphicsQueue() const;

    [[nodiscard]] VulkanLockedQueue LockPresentQueue() const;

private:
    class Impl;

    std::unique_ptr<Impl> m_impl;
};

} // namespace NVulkan
