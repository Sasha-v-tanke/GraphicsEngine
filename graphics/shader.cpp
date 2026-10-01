#include "shader.h"

#include <utility>

#include <GraphicsEngine/resources/resource_manager.h>
#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NGraphics {

Shader::Shader(NResources::ResourceManager& resources,
               NResources::ResourceHandle<NResources::ShaderArtifact> artifact,
               std::string entryPoint)
    : m_artifactResource(artifact)
    , m_artifactIdentity(resources.GetIdentity(artifact))
    , m_artifact(resources.GetCpuResource(artifact))
    , m_entryPoint(std::move(entryPoint)) {
    if (m_artifact == nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Shader artifact resource is not ready");
    }
}

} // namespace NGraphics
