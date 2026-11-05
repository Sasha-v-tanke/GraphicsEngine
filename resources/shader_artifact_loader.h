#pragma once

#include <filesystem>
#include <span>

#include <GraphicsEngine/resources/resource_handle.h>
#include <GraphicsEngine/resources/resource_identity.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <GraphicsEngine/resources/shader_artifact.h>

namespace NResources {

[[nodiscard]] std::shared_ptr<ShaderArtifact> DecodeShaderArtifact(std::span<const std::byte> bytes,
                                                                   EShaderStage stage);

[[nodiscard]] ResourceHandle<ShaderArtifact> LoadShaderArtifact(ResourceManager& resources,
                                                                ResourceIdentity identity,
                                                                const std::filesystem::path& path,
                                                                EShaderStage stage);

[[nodiscard]] bool IsValidSpirVArtifact(const std::vector<std::uint32_t>& words) noexcept;

} // namespace NResources
