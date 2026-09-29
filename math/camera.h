#pragma once

#include "matrix.h"
#include "vector.h"

namespace NMath {

struct PerspectiveProjection {
    float VerticalFovRadians = 0.0F;
    float AspectRatio = 1.0F;
    float NearPlane = 0.1F;
    float FarPlane = 1000.0F;
};

[[nodiscard]] Mat4 LookAt(Vec3 position, Vec3 target, Vec3 worldUp) noexcept;
[[nodiscard]] Mat4 Perspective(PerspectiveProjection projection) noexcept;

} // namespace NMath
