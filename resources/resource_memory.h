#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace NResources {

struct ResourceMemoryBudget {
    std::size_t MaxCpuBytes = std::numeric_limits<std::size_t>::max();
};

struct ResourceMemoryCounters {
    std::size_t DecodedBytes = 0;
    std::size_t CacheBytes = 0;
    std::size_t PendingDecodeBytes = 0;
    std::size_t PinnedBytes = 0;
    std::size_t EvictableBytes = 0;
    std::size_t HighWaterBytes = 0;
};

struct ResourceClassMemory {
    std::string ResourceClass;
    ResourceMemoryCounters Counters;
};

struct ResourceMemorySnapshot {
    ResourceMemoryBudget Budget;
    ResourceMemoryCounters Total;
    // Lexically sorted; pressure is derived from the current live charges, never sticky.
    std::vector<ResourceClassMemory> Classes;
    bool AtCapacity = false;
};

// Move-only scoped decode reservation. It may outlive the manager; release never calls the manager.
class ResourceMemoryReservation final {
public:
    ResourceMemoryReservation() = default;
    ResourceMemoryReservation(const ResourceMemoryReservation&) = delete;
    ResourceMemoryReservation& operator=(const ResourceMemoryReservation&) = delete;
    ResourceMemoryReservation(ResourceMemoryReservation&&) noexcept = default;
    ResourceMemoryReservation& operator=(ResourceMemoryReservation&&) noexcept = default;
    [[nodiscard]] bool IsValid() const noexcept {
        return static_cast<bool>(m_token);
    }

private:
    std::uint64_t m_owner = 0;
    std::string m_class;
    std::shared_ptr<const void> m_token;
    friend class ResourceManager;
};

// Customize for application payloads with dynamic storage. Counts owned CPU capacity, not allocator metadata.
inline std::size_t AddResourceMemory(std::size_t bytes, std::size_t count, std::size_t stride) {
    if (stride == 0) {
        return bytes;
    }
    if (count > (std::numeric_limits<std::size_t>::max() - bytes) / stride) {
        throw std::overflow_error("CPU resource size overflow");
    }
    return bytes + count * stride;
}

template<typename T>
struct ResourceMemorySize {
    static std::size_t GetBytes(const T&) {
        return sizeof(T);
    }
};

class ImageData;
class MeshData;
class ShaderArtifact;

template<>
struct ResourceMemorySize<ImageData> {
    static std::size_t GetBytes(const ImageData& data);
};
template<>
struct ResourceMemorySize<MeshData> {
    static std::size_t GetBytes(const MeshData& data);
};
template<>
struct ResourceMemorySize<ShaderArtifact> {
    static std::size_t GetBytes(const ShaderArtifact& data);
};

} // namespace NResources
