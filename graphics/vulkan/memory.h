#pragma once

#include <cstdint>
#include <span>
#include <vulkan.h>

#include <GraphicsEngine/graphics/buffer.h>
#include <GraphicsEngine/graphics/image.h>
#include <GraphicsEngine/lib/common/wrapper/non_copyable.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

#define VMA_KHR_EXTENDED_FLAGS 0
#define VMA_EXTERNAL_MEMORY_WIN32 0
#define VMA_NULLABLE
#define VMA_NOT_NULL
#define VMA_NOT_NULL_NON_DISPATCHABLE
#define VMA_NULLABLE_NON_DISPATCHABLE

#include <vk_mem_alloc.h>

namespace NVulkan {

class VulkanDevice;

class VulkanMemoryAllocator final: public NCommon::NonTransferable {
public:
    VulkanMemoryAllocator(VkInstance instance, VkPhysicalDevice physicalDevice, const VulkanDevice& device);

    ~VulkanMemoryAllocator();

    [[nodiscard]] VmaAllocator GetHandle() const noexcept;

private:
    VmaAllocator m_allocator = VK_NULL_HANDLE;
};

class VulkanBufferAllocation final: public NCommon::NonCopyable {
public:
    VulkanBufferAllocation() = default;
    VulkanBufferAllocation(VmaAllocator allocator,
                           VkBuffer buffer,
                           VmaAllocation allocation,
                           std::uint64_t sizeBytes,
                           VkBufferUsageFlags usage) noexcept;
    VulkanBufferAllocation(VulkanBufferAllocation&& other) noexcept;
    VulkanBufferAllocation& operator=(VulkanBufferAllocation&& other) noexcept;
    ~VulkanBufferAllocation();

    [[nodiscard]] VkBuffer GetBuffer() const noexcept;
    [[nodiscard]] std::uint64_t GetSizeBytes() const noexcept;
    [[nodiscard]] VkBufferUsageFlags GetUsage() const noexcept;
    [[nodiscard]] bool IsValid() const noexcept;

    [[nodiscard]] void* Map();
    void Unmap() noexcept;

private:
    void Reset() noexcept;

private:
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    std::uint64_t m_sizeBytes = 0;
    VkBufferUsageFlags m_usage = 0;
    bool m_mapped = false;
};

class VulkanImageAllocation final: public NCommon::NonCopyable {
public:
    VulkanImageAllocation() = default;
    VulkanImageAllocation(VmaAllocator allocator,
                          VkImage image,
                          VmaAllocation allocation,
                          const NGraphics::ImageDescriptor& descriptor,
                          VkImageUsageFlags usage) noexcept;
    VulkanImageAllocation(VulkanImageAllocation&& other) noexcept;
    VulkanImageAllocation& operator=(VulkanImageAllocation&& other) noexcept;
    ~VulkanImageAllocation();

    [[nodiscard]] VkImage GetImage() const noexcept;
    [[nodiscard]] const NGraphics::ImageDescriptor& GetDescriptor() const noexcept;
    [[nodiscard]] VkImageUsageFlags GetUsage() const noexcept;
    [[nodiscard]] bool IsValid() const noexcept;

private:
    void Reset() noexcept;

private:
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkImage m_image = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    NGraphics::ImageDescriptor m_descriptor;
    VkImageUsageFlags m_usage = 0;
};

[[nodiscard]] VulkanBufferAllocation CreateVulkanBuffer(VulkanMemoryAllocator& allocator,
                                                        const NGraphics::BufferDescriptor& descriptor);

[[nodiscard]] VulkanBufferAllocation CreateVulkanStagingBuffer(VulkanMemoryAllocator& allocator,
                                                               std::uint64_t sizeBytes);

[[nodiscard]] VulkanImageAllocation CreateVulkanImage(VulkanMemoryAllocator& allocator,
                                                      const NGraphics::ImageDescriptor& descriptor);

} // namespace NVulkan
