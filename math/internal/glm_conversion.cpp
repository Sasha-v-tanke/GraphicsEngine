#include "glm_conversion.h"

namespace NMath::NInternal {

glm::vec3 ToGlm(Vec3 vector) noexcept {
    return {vector.X, vector.Y, vector.Z};
}

glm::vec4 ToGlm(Vec4 vector) noexcept {
    return {vector.X, vector.Y, vector.Z, vector.W};
}

glm::quat ToGlm(Quat quaternion) noexcept {
    return {quaternion.W, quaternion.X, quaternion.Y, quaternion.Z};
}

glm::mat4 ToGlm(const Mat4& matrix) noexcept {
    const auto& values = matrix.GetValues();
    return glm::make_mat4(values.data());
}

Vec3 FromGlm(const glm::vec3& vector) noexcept {
    return {
            .X = vector.x,
            .Y = vector.y,
            .Z = vector.z,
    };
}

Vec4 FromGlm(const glm::vec4& vector) noexcept {
    return {
            .X = vector.x,
            .Y = vector.y,
            .Z = vector.z,
            .W = vector.w,
    };
}

Quat FromGlm(const glm::quat& quaternion) noexcept {
    return {
            .X = quaternion.x,
            .Y = quaternion.y,
            .Z = quaternion.z,
            .W = quaternion.w,
    };
}

Mat4 FromGlm(const glm::mat4& matrix) noexcept {
    Mat4::Values values{};
    const float* source = glm::value_ptr(matrix);

    for (std::size_t index = 0; index < values.size(); ++index) {
        values[index] = source[index];
    }

    return Mat4(values);
}

} // namespace NMath::NInternal
