#pragma once

#include <filesystem>

#include <GraphicsEngine/resources/resource_handle.h>
#include <GraphicsEngine/resources/resource_identity.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <GraphicsEngine/resources/shader_artifact.h>

namespace NResources {

[[nodiscard]] ResourceHandle<ShaderArtifact> LoadShaderArtifact(ResourceManager& resources,
                                                                ResourceIdentity identity,
                                                                const std::filesystem::path& path,
                                                                EShaderStage stage);

[[nodiscard]] bool IsValidSpirVArtifact(const std::vector<std::uint32_t>& words) noexcept;

} // namespace NResources
