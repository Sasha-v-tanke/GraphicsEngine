#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

#include <GraphicsEngine/resources/image_data.h>
#include <GraphicsEngine/resources/resource_handle.h>
#include <GraphicsEngine/resources/resource_identity.h>
#include <GraphicsEngine/resources/resource_manager.h>

namespace NResources {

[[nodiscard]] std::shared_ptr<ImageData> DecodeImage(std::span<const std::byte> bytes);

[[nodiscard]] ResourceHandle<ImageData>
LoadImage(ResourceManager& resources, ResourceIdentity identity, const std::filesystem::path& path);

} // namespace NResources
