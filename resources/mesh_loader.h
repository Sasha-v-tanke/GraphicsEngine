#pragma once

#include <filesystem>

#include <GraphicsEngine/resources/mesh_data.h>
#include <GraphicsEngine/resources/resource_handle.h>
#include <GraphicsEngine/resources/resource_identity.h>
#include <GraphicsEngine/resources/resource_manager.h>

namespace NResources {

// Synchronous CPU-only OBJ import. Never call on a frame worker: this performs blocking I/O.
// READY/LOADING/UNLOADING identities retain their current handle and payload; FAILED loads may retry.
// Coordinate system, winding and UV orientation are preserved; no attributes are synthesized.
[[nodiscard]] ResourceHandle<MeshData>
LoadMesh(ResourceManager& resources, ResourceIdentity identity, const std::filesystem::path& path);

} // namespace NResources
