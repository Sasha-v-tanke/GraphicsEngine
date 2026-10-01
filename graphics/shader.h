#pragma once

#include <memory>
#include <string>
#include <utility>

#include <GraphicsEngine/resources/resource_handle.h>
#include <GraphicsEngine/resources/resource_identity.h>
#include <GraphicsEngine/resources/shader_artifact.h>

namespace NResources {

class ResourceManager;

} // namespace NResources

namespace NGraphics {

class Shader final {
public:
    Shader() = default;

    Shader(NResources::ResourceManager& resources,
           NResources::ResourceHandle<NResources::ShaderArtifact> artifact,
           std::string entryPoint = "main");

    [[nodiscard]] bool IsValid() const noexcept {
        return m_artifactResource.IsValid() && m_artifactIdentity.IsValid() && m_artifact != nullptr;
    }

    [[nodiscard]] NResources::ResourceHandle<NResources::ShaderArtifact> GetArtifactResource() const noexcept {
        return m_artifactResource;
    }

    [[nodiscard]] const NResources::ResourceIdentity& GetArtifactIdentity() const noexcept {
        return m_artifactIdentity;
    }

    [[nodiscard]] const std::shared_ptr<const NResources::ShaderArtifact>& GetArtifact() const noexcept {
        return m_artifact;
    }

    [[nodiscard]] const std::string& GetEntryPoint() const noexcept {
        return m_entryPoint;
    }

    [[nodiscard]] friend bool operator==(const Shader& left, const Shader& right) noexcept {
        if (left.m_artifactResource != right.m_artifactResource ||
            left.m_artifactIdentity != right.m_artifactIdentity ||
            left.m_entryPoint != right.m_entryPoint) {
            return false;
        }

        if (left.m_artifact == right.m_artifact) {
            return true;
        }

        if (left.m_artifact == nullptr || right.m_artifact == nullptr) {
            return false;
        }

        return left.m_artifact->GetStage() == right.m_artifact->GetStage() &&
               left.m_artifact->GetWords() == right.m_artifact->GetWords();
    }

private:
    NResources::ResourceHandle<NResources::ShaderArtifact> m_artifactResource;
    NResources::ResourceIdentity m_artifactIdentity;
    std::shared_ptr<const NResources::ShaderArtifact> m_artifact;
    std::string m_entryPoint;
};

} // namespace NGraphics
