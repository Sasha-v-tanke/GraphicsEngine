#pragma once

#include "vector.h"

#include <array>
#include <cstddef>

namespace NMath {

class Mat4 {
public:
    using Values = std::array<float, 16>;

    constexpr Mat4() noexcept = default;

    explicit constexpr Mat4(Values values) noexcept
        : Values_(values) {
    }

    static constexpr Mat4 Identity() noexcept {
        return Mat4({
                1.0F,
                0.0F,
                0.0F,
                0.0F,
                0.0F,
                1.0F,
                0.0F,
                0.0F,
                0.0F,
                0.0F,
                1.0F,
                0.0F,
                0.0F,
                0.0F,
                0.0F,
                1.0F,
        });
    }

    constexpr float& operator()(std::size_t row, std::size_t column) noexcept {
        return Values_[column * 4 + row];
    }

    [[nodiscard]] constexpr float operator()(std::size_t row, std::size_t column) const noexcept {
        return Values_[column * 4 + row];
    }

    [[nodiscard]] constexpr const Values& GetValues() const noexcept {
        return Values_;
    }

private:
    Values Values_ = {};
};

[[nodiscard]] Mat4 operator*(const Mat4& left, const Mat4& right) noexcept;
[[nodiscard]] Vec4 operator*(const Mat4& matrix, Vec4 vector) noexcept;

} // namespace NMath
