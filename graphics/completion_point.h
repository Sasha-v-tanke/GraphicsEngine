#pragma once

#include <cstdint>

namespace NGraphics {

class CompletionPoint {
public:
    CompletionPoint() = default;

    explicit CompletionPoint(std::uint64_t value) noexcept
        : m_value(value) {
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return m_value != 0;
    }

    [[nodiscard]] std::uint64_t GetValue() const noexcept {
        return m_value;
    }

    [[nodiscard]] friend bool operator==(CompletionPoint left, CompletionPoint right) noexcept = default;

private:
    std::uint64_t m_value = 0;
};

} // namespace NGraphics
