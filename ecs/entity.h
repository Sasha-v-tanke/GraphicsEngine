#pragma once

#include <cstdint>

namespace NEcs {

struct Entity {
    static constexpr std::uint32_t INVALID_INDEX = UINT32_MAX;

    std::uint32_t Index = INVALID_INDEX;
    std::uint32_t Generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return Index != INVALID_INDEX;
    }

    [[nodiscard]] friend constexpr bool operator==(Entity left, Entity right) noexcept = default;
};

} // namespace NEcs
