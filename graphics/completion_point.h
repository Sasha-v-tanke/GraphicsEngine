#pragma once

#include <cstdint>

namespace NGraphics {

class CompletionPoint {
public:
    CompletionPoint() = default;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_ownerId != 0 && m_value != 0;
    }

    [[nodiscard]] std::uint64_t GetValue() const noexcept {
        return m_value;
    }

    [[nodiscard]] friend bool operator==(CompletionPoint left, CompletionPoint right) noexcept = default;

private:
    CompletionPoint(std::uint64_t ownerId, std::uint64_t value) noexcept
        : m_ownerId(ownerId)
        , m_value(value) {
    }

    std::uint64_t m_ownerId = 0;
    std::uint64_t m_value = 0;

    friend class Graphics;
};

} // namespace NGraphics
