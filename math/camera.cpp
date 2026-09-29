#include "camera.h"
#include "internal/glm_conversion.h"

#include <GraphicsEngine/external/glm/glm.h>

namespace NMath {

Mat4 LookAt(Vec3 position, Vec3 target, Vec3 worldUp) noexcept {
    return NInternal::FromGlm(
            glm::lookAt(NInternal::ToGlm(position), NInternal::ToGlm(target), NInternal::ToGlm(worldUp)));
}

Mat4 Perspective(PerspectiveProjection projection) noexcept {
    return NInternal::FromGlm(glm::perspective(projection.VerticalFovRadians,
                                               projection.AspectRatio,
                                               projection.NearPlane,
                                               projection.FarPlane));
}

} // namespace NMath
