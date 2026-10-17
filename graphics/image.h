#pragma once

#include <cstdint>

namespace NGraphics {

enum class EImageFormat {
    R8_UNORM,
    RG8_UNORM,
    RGBA8_UNORM,
    BGRA8_UNORM,
    D32_FLOAT,
};

enum class EImageUsage : std::uint32_t {
    TransferSource = 1U << 0U,
    TransferDestination = 1U << 1U,
    Sampled = 1U << 2U,
    ColorAttachment = 1U << 3U,
    DepthStencilAttachment = 1U << 4U,
};

enum class EImageAccess : std::uint32_t {
    GpuRead = 1U << 0U,
    GpuWrite = 1U << 1U,
};

enum class EImageAspect : std::uint32_t {
    Color = 1U << 0U,
    Depth = 1U << 1U,
};

enum class EImageLifetime {
    Persistent,
    FrameLocal,
};

enum class ESamplerFilter {
    Nearest,
    Linear,
};

enum class ESamplerAddressMode {
    Repeat,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder,
};

using ImageUsageFlags = std::uint32_t;
using ImageAccessFlags = std::uint32_t;
using ImageAspectFlags = std::uint32_t;

[[nodiscard]] constexpr ImageUsageFlags ImageUsage(EImageUsage usage) noexcept {
    return static_cast<ImageUsageFlags>(usage);
}

[[nodiscard]] constexpr ImageAccessFlags ImageAccess(EImageAccess access) noexcept {
    return static_cast<ImageAccessFlags>(access);
}

[[nodiscard]] constexpr ImageAspectFlags ImageAspect(EImageAspect aspect) noexcept {
    return static_cast<ImageAspectFlags>(aspect);
}

[[nodiscard]] constexpr ImageUsageFlags operator|(EImageUsage left, EImageUsage right) noexcept {
    return ImageUsage(left) | ImageUsage(right);
}

[[nodiscard]] constexpr ImageUsageFlags operator|(ImageUsageFlags left, EImageUsage right) noexcept {
    return left | ImageUsage(right);
}

[[nodiscard]] constexpr ImageAccessFlags operator|(EImageAccess left, EImageAccess right) noexcept {
    return ImageAccess(left) | ImageAccess(right);
}

[[nodiscard]] constexpr ImageAccessFlags operator|(ImageAccessFlags left, EImageAccess right) noexcept {
    return left | ImageAccess(right);
}

[[nodiscard]] constexpr ImageAspectFlags operator|(EImageAspect left, EImageAspect right) noexcept {
    return ImageAspect(left) | ImageAspect(right);
}

[[nodiscard]] constexpr ImageAspectFlags operator|(ImageAspectFlags left, EImageAspect right) noexcept {
    return left | ImageAspect(right);
}

struct ImageExtent {
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    std::uint32_t Depth = 1;
};

struct ImageDescriptor {
    ImageExtent Extent;
    std::uint32_t MipLevels = 1;
    std::uint32_t ArrayLayers = 1;
    EImageFormat Format = EImageFormat::RGBA8_UNORM;
    ImageUsageFlags Usage = 0;
    ImageAccessFlags Access = 0;
    EImageLifetime Lifetime = EImageLifetime::Persistent;
};

class ImageHandle {
public:
    ImageHandle() = default;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_ownerId != 0 && m_value != 0 && m_generation != 0;
    }

    [[nodiscard]] std::uint64_t GetValue() const noexcept {
        return m_value;
    }

    [[nodiscard]] std::uint64_t GetGeneration() const noexcept {
        return m_generation;
    }

    [[nodiscard]] friend bool operator==(ImageHandle left, ImageHandle right) noexcept = default;

private:
    ImageHandle(std::uint64_t ownerId, std::uint64_t value, std::uint64_t generation) noexcept
        : m_ownerId(ownerId)
        , m_value(value)
        , m_generation(generation) {
    }

    std::uint64_t m_ownerId = 0;
    std::uint64_t m_value = 0;
    std::uint64_t m_generation = 0;

    friend class Graphics;
};

struct ImageViewDescriptor {
    ImageHandle Image;
    EImageFormat Format = EImageFormat::RGBA8_UNORM;
    ImageAspectFlags Aspects = 0;
    std::uint32_t BaseMipLevel = 0;
    std::uint32_t LevelCount = 1;
    std::uint32_t BaseArrayLayer = 0;
    std::uint32_t LayerCount = 1;
};

class ImageViewHandle {
public:
    ImageViewHandle() = default;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_ownerId != 0 && m_value != 0 && m_generation != 0;
    }

    [[nodiscard]] std::uint64_t GetValue() const noexcept {
        return m_value;
    }

    [[nodiscard]] std::uint64_t GetGeneration() const noexcept {
        return m_generation;
    }

    [[nodiscard]] friend bool operator==(ImageViewHandle left, ImageViewHandle right) noexcept = default;

private:
    ImageViewHandle(std::uint64_t ownerId, std::uint64_t value, std::uint64_t generation) noexcept
        : m_ownerId(ownerId)
        , m_value(value)
        , m_generation(generation) {
    }

    std::uint64_t m_ownerId = 0;
    std::uint64_t m_value = 0;
    std::uint64_t m_generation = 0;

    friend class Graphics;
};

struct SamplerDescriptor {
    ESamplerFilter MinFilter = ESamplerFilter::Linear;
    ESamplerFilter MagFilter = ESamplerFilter::Linear;
    ESamplerAddressMode AddressModeU = ESamplerAddressMode::Repeat;
    ESamplerAddressMode AddressModeV = ESamplerAddressMode::Repeat;
    ESamplerAddressMode AddressModeW = ESamplerAddressMode::Repeat;
    float MinLod = 0.0F;
    float MaxLod = 0.0F;
};

class SamplerHandle {
public:
    SamplerHandle() = default;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_ownerId != 0 && m_value != 0 && m_generation != 0;
    }

    [[nodiscard]] std::uint64_t GetValue() const noexcept {
        return m_value;
    }

    [[nodiscard]] std::uint64_t GetGeneration() const noexcept {
        return m_generation;
    }

    [[nodiscard]] friend bool operator==(SamplerHandle left, SamplerHandle right) noexcept = default;

private:
    SamplerHandle(std::uint64_t ownerId, std::uint64_t value, std::uint64_t generation) noexcept
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
