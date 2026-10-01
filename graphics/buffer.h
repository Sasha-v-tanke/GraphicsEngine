#pragma once

#include <cstdint>

namespace NGraphics {

enum class EBufferUsage : std::uint32_t {
    TransferSource = 1U << 0U,
    TransferDestination = 1U << 1U,
    Vertex = 1U << 2U,
    Index = 1U << 3U,
    Uniform = 1U << 4U,
    Storage = 1U << 5U,
};

enum class EBufferAccess : std::uint32_t {
    CpuRead = 1U << 0U,
    CpuWrite = 1U << 1U,
    GpuRead = 1U << 2U,
    GpuWrite = 1U << 3U,
};

enum class EBufferLifetime {
    Persistent,
    FrameLocal,
};

using BufferUsageFlags = std::uint32_t;
using BufferAccessFlags = std::uint32_t;

[[nodiscard]] constexpr BufferUsageFlags BufferUsage(EBufferUsage usage) noexcept {
    return static_cast<BufferUsageFlags>(usage);
}

[[nodiscard]] constexpr BufferAccessFlags BufferAccess(EBufferAccess access) noexcept {
    return static_cast<BufferAccessFlags>(access);
}

[[nodiscard]] constexpr BufferUsageFlags operator|(EBufferUsage left, EBufferUsage right) noexcept {
    return BufferUsage(left) | BufferUsage(right);
}

[[nodiscard]] constexpr BufferUsageFlags operator|(BufferUsageFlags left, EBufferUsage right) noexcept {
    return left | BufferUsage(right);
}

[[nodiscard]] constexpr BufferAccessFlags operator|(EBufferAccess left, EBufferAccess right) noexcept {
    return BufferAccess(left) | BufferAccess(right);
}

[[nodiscard]] constexpr BufferAccessFlags operator|(BufferAccessFlags left, EBufferAccess right) noexcept {
    return left | BufferAccess(right);
}

struct BufferDescriptor {
    std::uint64_t SizeBytes = 0;
    BufferUsageFlags Usage = 0;
    BufferAccessFlags Access = 0;
    EBufferLifetime Lifetime = EBufferLifetime::Persistent;
};

class BufferHandle {
public:
    BufferHandle() = default;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_ownerId != 0 && m_value != 0 && m_generation != 0;
    }

    [[nodiscard]] std::uint64_t GetValue() const noexcept {
        return m_value;
    }

    [[nodiscard]] std::uint64_t GetGeneration() const noexcept {
        return m_generation;
    }

    [[nodiscard]] friend bool operator==(BufferHandle left, BufferHandle right) noexcept = default;

private:
    BufferHandle(std::uint64_t ownerId, std::uint64_t value, std::uint64_t generation) noexcept
        : m_ownerId(ownerId)
        , m_value(value)
        , m_generation(generation) {
    }

    std::uint64_t m_ownerId = 0;
    std::uint64_t m_value = 0;
    std::uint64_t m_generation = 0;

    friend class Graphics;
};

} // namespace NGraphics
