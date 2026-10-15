#include "device.h"
#include "memory.h"
#include "resource_conversion.h"

#include <cstring>
#include <string_view>
#include <utility>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan {

namespace {

std::string_view GetVkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    default:
        return "VK_RESULT_UNKNOWN";
    }
}

[[nodiscard]] VmaAllocationCreateFlags MakeAllocationFlags(NGraphics::BufferAccessFlags access) noexcept {
    VmaAllocationCreateFlags flags = 0;

    if ((access & NGraphics::BufferAccess(NGraphics::EBufferAccess::CpuWrite)) != 0) {
        flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }

    if ((access & NGraphics::BufferAccess(NGraphics::EBufferAccess::CpuRead)) != 0) {
        flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }

    return flags;
}

[[nodiscard]] VmaMemoryUsage MakeBufferMemoryUsage(NGraphics::BufferAccessFlags access) noexcept {
    if ((access & (NGraphics::BufferAccess(NGraphics::EBufferAccess::CpuRead) |
                   NGraphics::BufferAccess(NGraphics::EBufferAccess::CpuWrite))) != 0) {
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    }

    return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
}

[[nodiscard]] VmaMemoryUsage MakeImageMemoryUsage(NGraphics::ImageAccessFlags) noexcept {
    return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
}

} // namespace

VulkanMemoryAllocator::VulkanMemoryAllocator(VkInstance instance,
                                             VkPhysicalDevice physicalDevice,
                                             const VulkanDevice& device,
                                             VulkanResourceStateTracker& resourceStates)
    : m_resourceStates(resourceStates) {
    if (instance == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan memory allocator requires an instance");
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan memory allocator requires a physical device");
    }

    if (device.GetHandle() == VK_NULL_HANDLE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan memory allocator requires a device");
    }

    const VmaVulkanFunctions functions{
            .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
    };
    const VmaAllocatorCreateInfo createInfo{
            .flags = 0,
            .physicalDevice = physicalDevice,
            .device = device.GetHandle(),
            .preferredLargeHeapBlockSize = 0,
            .pAllocationCallbacks = nullptr,
            .pDeviceMemoryCallbacks = nullptr,
            .pHeapSizeLimit = nullptr,
            .pVulkanFunctions = &functions,
            .instance = instance,
            .vulkanApiVersion = VK_API_VERSION_1_3,
            .pTypeExternalMemoryHandleTypes = nullptr,
    };

    const VkResult result = vmaCreateAllocator(&createInfo, &m_allocator);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to create Vulkan memory allocator: {}",
                              GetVkResultName(result));
    }
}

VulkanMemoryAllocator::~VulkanMemoryAllocator() {
    if (m_allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(m_allocator);
        m_allocator = VK_NULL_HANDLE;
    }
}

VmaAllocator VulkanMemoryAllocator::GetHandle() const noexcept {
    return m_allocator;
}

VulkanResourceStateTracker& VulkanMemoryAllocator::GetResourceStates() const noexcept {
    return m_resourceStates;
}

struct VulkanBufferAllocation::Lifetime {
    VmaAllocator Allocator = VK_NULL_HANDLE;
    VulkanResourceStateTracker* ResourceStates = nullptr;
    VkBuffer Buffer = VK_NULL_HANDLE;
    VmaAllocation Allocation = VK_NULL_HANDLE;

    Lifetime(VmaAllocator allocator,
             VulkanResourceStateTracker& resourceStates,
             VkBuffer buffer,
             VmaAllocation allocation) noexcept
        : Allocator(allocator)
        , ResourceStates(&resourceStates)
        , Buffer(buffer)
        , Allocation(allocation) {
    }

    Lifetime(const Lifetime&) = delete;

    ~Lifetime() {
        if (Allocator != VK_NULL_HANDLE && Buffer != VK_NULL_HANDLE) {
            ResourceStates->ForgetBuffer(Buffer);
            vmaDestroyBuffer(Allocator, Buffer, Allocation);
        }
    }
};

VulkanBufferAllocation::VulkanBufferAllocation(VmaAllocator allocator,
                                               VulkanResourceStateTracker& resourceStates,
                                               VkBuffer buffer,
                                               VmaAllocation allocation,
                                               std::uint64_t sizeBytes,
                                               VkBufferUsageFlags usage)
    : m_allocator(allocator)
    , m_lifetime(std::make_shared<Lifetime>(allocator, resourceStates, buffer, allocation))
    , m_buffer(buffer)
    , m_allocation(allocation)
    , m_sizeBytes(sizeBytes)
    , m_usage(usage) {
}

VulkanBufferAllocation::VulkanBufferAllocation(VulkanBufferAllocation&& other) noexcept
    : m_allocator(std::exchange(other.m_allocator, VK_NULL_HANDLE))
    , m_lifetime(std::move(other.m_lifetime))
    , m_buffer(std::exchange(other.m_buffer, VK_NULL_HANDLE))
    , m_allocation(std::exchange(other.m_allocation, VK_NULL_HANDLE))
    , m_sizeBytes(std::exchange(other.m_sizeBytes, 0))
    , m_usage(std::exchange(other.m_usage, 0))
    , m_mapped(std::exchange(other.m_mapped, false)) {
}

VulkanBufferAllocation& VulkanBufferAllocation::operator=(VulkanBufferAllocation&& other) noexcept {
    if (this != &other) {
        Reset();
        m_allocator = std::exchange(other.m_allocator, VK_NULL_HANDLE);
        m_lifetime = std::move(other.m_lifetime);
        m_buffer = std::exchange(other.m_buffer, VK_NULL_HANDLE);
        m_allocation = std::exchange(other.m_allocation, VK_NULL_HANDLE);
        m_sizeBytes = std::exchange(other.m_sizeBytes, 0);
        m_usage = std::exchange(other.m_usage, 0);
        m_mapped = std::exchange(other.m_mapped, false);
    }

    return *this;
}

VulkanBufferAllocation::~VulkanBufferAllocation() {
    Reset();
}

VkBuffer VulkanBufferAllocation::GetBuffer() const noexcept {
    return m_buffer;
}

std::uint64_t VulkanBufferAllocation::GetSizeBytes() const noexcept {
    return m_sizeBytes;
}

VkBufferUsageFlags VulkanBufferAllocation::GetUsage() const noexcept {
    return m_usage;
}

bool VulkanBufferAllocation::IsValid() const noexcept {
    return m_allocator != VK_NULL_HANDLE && m_buffer != VK_NULL_HANDLE && m_allocation != VK_NULL_HANDLE;
}

void* VulkanBufferAllocation::Map() {
    if (!IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Cannot map invalid Vulkan buffer allocation");
    }

    if (m_mapped) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Vulkan buffer allocation is already mapped");
    }

    void* data = nullptr;
    const VkResult result = vmaMapMemory(m_allocator, m_allocation, &data);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to map Vulkan buffer allocation: {}",
                              GetVkResultName(result));
    }

    m_mapped = true;

    return data;
}

void VulkanBufferAllocation::Flush(std::uint64_t offsetBytes, std::uint64_t sizeBytes) {
    if (!m_mapped) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Vulkan buffer allocation must be mapped before flushing");
    }

    if (offsetBytes > m_sizeBytes || sizeBytes > m_sizeBytes - offsetBytes) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan buffer flush range is out of bounds");
    }

    const VkResult result = vmaFlushAllocation(m_allocator, m_allocation, offsetBytes, sizeBytes);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to flush Vulkan buffer allocation: {}",
                              GetVkResultName(result));
    }
}

void VulkanBufferAllocation::Invalidate(std::uint64_t offsetBytes, std::uint64_t sizeBytes) {
    if (!m_mapped) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Vulkan buffer allocation must be mapped before invalidating");
    }

    if (offsetBytes > m_sizeBytes || sizeBytes > m_sizeBytes - offsetBytes) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan buffer invalidate range is out of bounds");
    }

    const VkResult result = vmaInvalidateAllocation(m_allocator, m_allocation, offsetBytes, sizeBytes);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to invalidate Vulkan buffer allocation: {}",
                              GetVkResultName(result));
    }
}

void VulkanBufferAllocation::Unmap() noexcept {
    if (m_mapped) {
        vmaUnmapMemory(m_allocator, m_allocation);
        m_mapped = false;
    }
}

std::shared_ptr<void> VulkanBufferAllocation::GetRetentionToken() const noexcept {
    return m_lifetime;
}

void VulkanBufferAllocation::Reset() noexcept {
    Unmap();

    m_allocator = VK_NULL_HANDLE;
    m_lifetime.reset();
    m_buffer = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_sizeBytes = 0;
    m_usage = 0;
}

struct VulkanImageAllocation::Lifetime {
    VmaAllocator Allocator = VK_NULL_HANDLE;
    VulkanResourceStateTracker* ResourceStates = nullptr;
    VkImage Image = VK_NULL_HANDLE;
    VmaAllocation Allocation = VK_NULL_HANDLE;

    Lifetime(VmaAllocator allocator,
             VulkanResourceStateTracker& resourceStates,
             VkImage image,
             VmaAllocation allocation) noexcept
        : Allocator(allocator)
        , ResourceStates(&resourceStates)
        , Image(image)
        , Allocation(allocation) {
    }

    Lifetime(const Lifetime&) = delete;

    ~Lifetime() {
        if (Allocator != VK_NULL_HANDLE && Image != VK_NULL_HANDLE) {
            ResourceStates->ForgetImage(Image);
            vmaDestroyImage(Allocator, Image, Allocation);
        }
    }
};

VulkanImageAllocation::VulkanImageAllocation(VmaAllocator allocator,
                                             VulkanResourceStateTracker& resourceStates,
                                             VkImage image,
                                             VmaAllocation allocation,
                                             const NGraphics::ImageDescriptor& descriptor,
                                             VkImageUsageFlags usage)
    : m_allocator(allocator)
    , m_lifetime(std::make_shared<Lifetime>(allocator, resourceStates, image, allocation))
    , m_image(image)
    , m_allocation(allocation)
    , m_descriptor(descriptor)
    , m_usage(usage) {
}

VulkanImageAllocation::VulkanImageAllocation(VulkanImageAllocation&& other) noexcept
    : m_allocator(std::exchange(other.m_allocator, VK_NULL_HANDLE))
    , m_lifetime(std::move(other.m_lifetime))
    , m_image(std::exchange(other.m_image, VK_NULL_HANDLE))
    , m_allocation(std::exchange(other.m_allocation, VK_NULL_HANDLE))
    , m_descriptor(other.m_descriptor)
    , m_usage(std::exchange(other.m_usage, 0)) {
}

VulkanImageAllocation& VulkanImageAllocation::operator=(VulkanImageAllocation&& other) noexcept {
    if (this != &other) {
        Reset();
        m_allocator = std::exchange(other.m_allocator, VK_NULL_HANDLE);
        m_lifetime = std::move(other.m_lifetime);
        m_image = std::exchange(other.m_image, VK_NULL_HANDLE);
        m_allocation = std::exchange(other.m_allocation, VK_NULL_HANDLE);
        m_descriptor = other.m_descriptor;
        m_usage = std::exchange(other.m_usage, 0);
    }

    return *this;
}

VulkanImageAllocation::~VulkanImageAllocation() {
    Reset();
}

VkImage VulkanImageAllocation::GetImage() const noexcept {
    return m_image;
}

const NGraphics::ImageDescriptor& VulkanImageAllocation::GetDescriptor() const noexcept {
    return m_descriptor;
}

VkImageUsageFlags VulkanImageAllocation::GetUsage() const noexcept {
    return m_usage;
}

bool VulkanImageAllocation::IsValid() const noexcept {
    return m_allocator != VK_NULL_HANDLE && m_image != VK_NULL_HANDLE && m_allocation != VK_NULL_HANDLE;
}

std::shared_ptr<void> VulkanImageAllocation::GetRetentionToken() const noexcept {
    return m_lifetime;
}

void VulkanImageAllocation::Reset() noexcept {
    m_allocator = VK_NULL_HANDLE;
    m_lifetime.reset();
    m_image = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_descriptor = {};
    m_usage = 0;
}

VulkanBufferAllocation CreateVulkanBuffer(VulkanMemoryAllocator& allocator,
                                          const NGraphics::BufferDescriptor& descriptor) {
    if (descriptor.SizeBytes == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan buffer allocation size must be non-zero");
    }

    const VkBufferUsageFlags usage = ToVulkanBufferUsage(descriptor.Usage);
    const VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = descriptor.SizeBytes,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
    };
    const VmaAllocationCreateInfo allocationInfo{
            .flags = MakeAllocationFlags(descriptor.Access),
            .usage = MakeBufferMemoryUsage(descriptor.Access),
            .requiredFlags = 0,
            .preferredFlags = 0,
            .memoryTypeBits = 0,
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
            .priority = 0.0F,
    };

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    const VkResult result =
            vmaCreateBuffer(allocator.GetHandle(), &bufferInfo, &allocationInfo, &buffer, &allocation, nullptr);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to allocate Vulkan buffer: {}",
                              GetVkResultName(result));
    }

    VulkanBufferAllocation bufferAllocation{allocator.GetHandle(),
                                            allocator.GetResourceStates(),
                                            buffer,
                                            allocation,
                                            descriptor.SizeBytes,
                                            usage};
    allocator.GetResourceStates().ImportBuffer(buffer,
                                               0,
                                               descriptor.SizeBytes,
                                               MakeVulkanBufferState(EVulkanResourceUsage::Undefined));
    return bufferAllocation;
}

VulkanBufferAllocation CreateVulkanStagingBuffer(VulkanMemoryAllocator& allocator, std::uint64_t sizeBytes) {
    VulkanBufferAllocation staging =
            CreateVulkanBuffer(allocator,
                               NGraphics::BufferDescriptor{
                                       .SizeBytes = sizeBytes,
                                       .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::TransferSource),
                                       .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::CpuWrite),
                                       .Lifetime = NGraphics::EBufferLifetime::FrameLocal,
                               });
    allocator.GetResourceStates().ForgetBuffer(staging.GetBuffer());
    return staging;
}

VulkanImageAllocation CreateVulkanImage(VulkanMemoryAllocator& allocator,
                                        const NGraphics::ImageDescriptor& descriptor) {
    const VkFormat format = ToVulkanFormat(descriptor.Format);
    const VkImageUsageFlags usage = ToVulkanImageUsage(descriptor.Usage);
    const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent =
                    {
                            .width = descriptor.Extent.Width,
                            .height = descriptor.Extent.Height,
                            .depth = descriptor.Extent.Depth,
                    },
            .mipLevels = descriptor.MipLevels,
            .arrayLayers = descriptor.ArrayLayers,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocationInfo{
            .flags = 0,
            .usage = MakeImageMemoryUsage(descriptor.Access),
            .requiredFlags = 0,
            .preferredFlags = 0,
            .memoryTypeBits = 0,
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
            .priority = 0.0F,
    };

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    const VkResult result =
            vmaCreateImage(allocator.GetHandle(), &imageInfo, &allocationInfo, &image, &allocation, nullptr);
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Failed to allocate Vulkan image: {}",
                              GetVkResultName(result));
    }

    return VulkanImageAllocation{allocator.GetHandle(),
                                 allocator.GetResourceStates(),
                                 image,
                                 allocation,
                                 descriptor,
                                 usage};
}

} // namespace NVulkan
