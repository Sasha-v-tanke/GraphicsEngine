#pragma once

#include <GraphicsEngine/math/camera.h>
#include <GraphicsEngine/math/transform.h>
#include <GraphicsEngine/resources/resource_handle.h>

namespace NGraphics {

class Material;

} // namespace NGraphics

namespace NResources {

class MeshData;

} // namespace NResources

namespace NRenderer {

// MVP policy: omit the entire object if any required logical resource is unavailable.
// Fallback selection can extend this policy without changing Resources or backend APIs.
enum class EResourceReadinessPolicy {
    SKIP_OBJECT,
};

struct TransformComponent {
    NMath::Transform Value = {};
};

// Positive FOV selects perspective projection from the camera's inverse world transform.
// Zero FOV is a clip-space view (identity view/projection), retained for simple 2D samples.
struct CameraComponent {
    NMath::PerspectiveProjection Projection = {};
};

struct RenderableComponent {
    NResources::ResourceHandle<NResources::MeshData> Mesh;
    NResources::ResourceHandle<NGraphics::Material> Material;
    EResourceReadinessPolicy ReadinessPolicy = EResourceReadinessPolicy::SKIP_OBJECT;
};

} // namespace NRenderer
