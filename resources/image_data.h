#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace NResources {

enum class EImagePixelFormat {
    RGBA8,
};

class ImageData final {
public:
    ImageData() = default;

    ImageData(std::uint32_t width, std::uint32_t height, EImagePixelFormat format, std::vector<std::uint8_t> pixels)
        : m_width(width)
        , m_height(height)
        , m_format(format)
        , m_pixels(std::move(pixels)) {
    }

    [[nodiscard]] std::uint32_t GetWidth() const noexcept {
        return m_width;
    }

    [[nodiscard]] std::uint32_t GetHeight() const noexcept {
        return m_height;
    }

    [[nodiscard]] EImagePixelFormat GetFormat() const noexcept {
        return m_format;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& GetPixels() const noexcept {
        return m_pixels;
    }

private:
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    EImagePixelFormat m_format = EImagePixelFormat::RGBA8;
    std::vector<std::uint8_t> m_pixels;
};

} // namespace NResources
