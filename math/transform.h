#pragma once

#include "matrix.h"
#include "quaternion.h"
#include "vector.h"

namespace NMath {

struct Transform {
    Vec3 Translation = {};
    Quat Rotation = {};
    Vec3 Scale = {
            .X = 1.0F,
            .Y = 1.0F,
            .Z = 1.0F,
    };
};

[[nodiscard]] Mat4 ComposeTransform(const Transform& transform) noexcept;

} // namespace NMath
