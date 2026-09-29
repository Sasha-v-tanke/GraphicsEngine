#include "internal/glm_conversion.h"
#include "vector.h"

#include <GraphicsEngine/external/glm/glm.h>

namespace NMath {

float Dot(Vec3 left, Vec3 right) noexcept {
    return glm::dot(NInternal::ToGlm(left), NInternal::ToGlm(right));
}

Vec3 Cross(Vec3 left, Vec3 right) noexcept {
    return NInternal::FromGlm(glm::cross(NInternal::ToGlm(left), NInternal::ToGlm(right)));
}

float Length(Vec3 vector) noexcept {
    return glm::length(NInternal::ToGlm(vector));
}

Vec3 Normalize(Vec3 vector) noexcept {
    return NInternal::FromGlm(glm::normalize(NInternal::ToGlm(vector)));
}

} // namespace NMath
