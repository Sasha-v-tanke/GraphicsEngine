#pragma once

#include <GraphicsEngine/math/camera.h>
#include <GraphicsEngine/math/transform.h>
#include <GraphicsEngine/resources/resource_handle.h>

namespace NResources {

class Material;
class MeshData;

} // namespace NResources

namespace NRenderer {

struct TransformComponent {
    NMath::Transform Value = {};
};

struct CameraComponent {
    NMath::PerspectiveProjection Projection = {};
};

struct RenderableComponent {
    NResources::ResourceHandle<NResources::MeshData> Mesh;
    NResources::ResourceHandle<NResources::Material> Material;
};

} // namespace NRenderer
