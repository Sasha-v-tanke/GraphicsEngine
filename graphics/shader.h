#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace NGraphics {

enum class EShaderStage {
    VERTEX,
    FRAGMENT,
    COMPUTE,
};

enum class EShaderArtifactFormat {
    SPIR_V,
};

struct ShaderArtifact {
    EShaderArtifactFormat Format = EShaderArtifactFormat::SPIR_V;
    std::vector<std::uint32_t> Words;

    [[nodiscard]] friend bool operator==(const ShaderArtifact& left, const ShaderArtifact& right) noexcept = default;
};

class Shader final {
public:
    Shader() = default;

    Shader(EShaderStage stage, ShaderArtifact artifact, std::string entryPoint = "main")
        : m_stage(stage)
        , m_artifact(std::move(artifact))
        , m_entryPoint(std::move(entryPoint)) {
    }

    [[nodiscard]] EShaderStage GetStage() const noexcept {
        return m_stage;
    }

    [[nodiscard]] const ShaderArtifact& GetArtifact() const noexcept {
        return m_artifact;
    }

    [[nodiscard]] const std::string& GetEntryPoint() const noexcept {
        return m_entryPoint;
    }

    [[nodiscard]] friend bool operator==(const Shader& left, const Shader& right) noexcept = default;

private:
    EShaderStage m_stage = EShaderStage::VERTEX;
    ShaderArtifact m_artifact;
    std::string m_entryPoint;
};

} // namespace NGraphics
