#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vulkan.h>

#include <GraphicsEngine/graphics/buffer.h>
#include <GraphicsEngine/graphics/image.h>
#include <GraphicsEngine/graphics/vulkan/resource_state.h>
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
    VulkanMemoryAllocator(VkInstance instance,
                          VkPhysicalDevice physicalDevice,
                          const VulkanDevice& device,
                          VulkanResourceStateTracker& resourceStates);

    ~VulkanMemoryAllocator();

    [[nodiscard]] VmaAllocator GetHandle() const noexcept;
    [[nodiscard]] VulkanResourceStateTracker& GetResourceStates() const noexcept;

private:
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VulkanResourceStateTracker& m_resourceStates;
};

class VulkanBufferAllocation final: public NCommon::NonCopyable {
public:
    VulkanBufferAllocation() = default;
    VulkanBufferAllocation(VmaAllocator allocator,
                           VulkanResourceStateTracker& resourceStates,
                           VkBuffer buffer,
                           VmaAllocation allocation,
                           std::uint64_t sizeBytes,
                           VkBufferUsageFlags usage);
    VulkanBufferAllocation(VulkanBufferAllocation&& other) noexcept;
    VulkanBufferAllocation& operator=(VulkanBufferAllocation&& other) noexcept;
    ~VulkanBufferAllocation();

    [[nodiscard]] VkBuffer GetBuffer() const noexcept;
    [[nodiscard]] std::uint64_t GetSizeBytes() const noexcept;
    [[nodiscard]] VkBufferUsageFlags GetUsage() const noexcept;
    [[nodiscard]] bool IsValid() const noexcept;

    [[nodiscard]] void* Map();
    void Flush(std::uint64_t offsetBytes, std::uint64_t sizeBytes);
    void Invalidate(std::uint64_t offsetBytes, std::uint64_t sizeBytes);
    void Unmap() noexcept;

    [[nodiscard]] std::shared_ptr<void> GetRetentionToken() const noexcept;

private:
    struct Lifetime;

    void Reset() noexcept;

private:
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    std::shared_ptr<Lifetime> m_lifetime;
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
                          VulkanResourceStateTracker& resourceStates,
                          VkImage image,
                          VmaAllocation allocation,
                          const NGraphics::ImageDescriptor& descriptor,
                          VkImageUsageFlags usage);
    VulkanImageAllocation(VulkanImageAllocation&& other) noexcept;
    VulkanImageAllocation& operator=(VulkanImageAllocation&& other) noexcept;
    ~VulkanImageAllocation();

    [[nodiscard]] VkImage GetImage() const noexcept;
    [[nodiscard]] const NGraphics::ImageDescriptor& GetDescriptor() const noexcept;
    [[nodiscard]] VkImageUsageFlags GetUsage() const noexcept;
    [[nodiscard]] bool IsValid() const noexcept;

    [[nodiscard]] std::shared_ptr<void> GetRetentionToken() const noexcept;

private:
    struct Lifetime;

    void Reset() noexcept;

private:
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    std::shared_ptr<Lifetime> m_lifetime;
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
