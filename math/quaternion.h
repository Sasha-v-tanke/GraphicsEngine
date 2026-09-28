#pragma once

#include "matrix.h"
#include "vector.h"

namespace NMath {

struct Quat {
    float X = 0.0F;
    float Y = 0.0F;
    float Z = 0.0F;
    float W = 1.0F;
};

[[nodiscard]] Quat Normalize(Quat quaternion) noexcept;
[[nodiscard]] Quat AngleAxis(float radians, Vec3 axis) noexcept;
[[nodiscard]] Quat operator*(Quat left, Quat right) noexcept;
[[nodiscard]] Mat4 ToMatrix(Quat quaternion) noexcept;

} // namespace NMath
