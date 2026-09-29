#pragma once

#include "../matrix.h"
#include "../quaternion.h"
#include "../vector.h"

#include <GraphicsEngine/external/glm/glm.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace NMath::NInternal {

[[nodiscard]] glm::vec3 ToGlm(Vec3 vector) noexcept;
[[nodiscard]] glm::vec4 ToGlm(Vec4 vector) noexcept;
[[nodiscard]] glm::quat ToGlm(Quat quaternion) noexcept;
[[nodiscard]] glm::mat4 ToGlm(const Mat4& matrix) noexcept;

[[nodiscard]] Vec3 FromGlm(const glm::vec3& vector) noexcept;
[[nodiscard]] Vec4 FromGlm(const glm::vec4& vector) noexcept;
[[nodiscard]] Quat FromGlm(const glm::quat& quaternion) noexcept;
[[nodiscard]] Mat4 FromGlm(const glm::mat4& matrix) noexcept;

} // namespace NMath::NInternal
