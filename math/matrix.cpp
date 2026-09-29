#include "internal/glm_conversion.h"
#include "matrix.h"

#include <GraphicsEngine/external/glm/glm.h>

namespace NMath {

Mat4 operator*(const Mat4& left, const Mat4& right) noexcept {
    return NInternal::FromGlm(NInternal::ToGlm(left) * NInternal::ToGlm(right));
}

Vec4 operator*(const Mat4& matrix, Vec4 vector) noexcept {
    return NInternal::FromGlm(NInternal::ToGlm(matrix) * NInternal::ToGlm(vector));
}

} // namespace NMath
