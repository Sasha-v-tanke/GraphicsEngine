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

struct TransformComponent {
    NMath::Transform Value = {};
};

struct CameraComponent {
    NMath::PerspectiveProjection Projection = {};
};

struct RenderableComponent {
    NResources::ResourceHandle<NResources::MeshData> Mesh;
    NResources::ResourceHandle<NGraphics::Material> Material;
};

} // namespace NRenderer
