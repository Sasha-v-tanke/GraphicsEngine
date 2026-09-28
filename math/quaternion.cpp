#include "internal/glm_conversion.h"
#include "quaternion.h"

#include <GraphicsEngine/external/glm/glm.h>

namespace NMath {

Quat Normalize(Quat quaternion) noexcept {
    return NInternal::FromGlm(glm::normalize(NInternal::ToGlm(quaternion)));
}

Quat AngleAxis(float radians, Vec3 axis) noexcept {
    return NInternal::FromGlm(glm::angleAxis(radians, glm::normalize(NInternal::ToGlm(axis))));
}

Quat operator*(Quat left, Quat right) noexcept {
    return NInternal::FromGlm(NInternal::ToGlm(left) * NInternal::ToGlm(right));
}

Mat4 ToMatrix(Quat quaternion) noexcept {
    return NInternal::FromGlm(glm::mat4_cast(NInternal::ToGlm(quaternion)));
}

} // namespace NMath
