#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

#include <GraphicsEngine/resources/mesh_data.h>
#include <GraphicsEngine/resources/resource_handle.h>
#include <GraphicsEngine/resources/resource_identity.h>
#include <GraphicsEngine/resources/resource_manager.h>

namespace NResources {

[[nodiscard]] std::shared_ptr<MeshData> DecodeMesh(std::span<const std::byte> bytes);

// Synchronous CPU-only OBJ import. Never call on a frame worker: this performs blocking I/O.
// READY/LOADING/UNLOADING identities retain their current handle and payload; FAILED loads may retry.
// Coordinate system, winding and UV orientation are preserved; no attributes are synthesized.
[[nodiscard]] ResourceHandle<MeshData>
LoadMesh(ResourceManager& resources, ResourceIdentity identity, const std::filesystem::path& path);

} // namespace NResources
