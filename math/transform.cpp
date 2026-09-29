#include "internal/glm_conversion.h"
#include "transform.h"

#include <GraphicsEngine/external/glm/glm.h>

namespace NMath {

Mat4 ComposeTransform(const Transform& transform) noexcept {
    const glm::mat4 translation = glm::translate(glm::mat4(1.0F), NInternal::ToGlm(transform.Translation));
    const glm::mat4 rotation = glm::mat4_cast(NInternal::ToGlm(transform.Rotation));
    const glm::mat4 scale = glm::scale(glm::mat4(1.0F), NInternal::ToGlm(transform.Scale));

    return NInternal::FromGlm(translation * rotation * scale);
}

} // namespace NMath
