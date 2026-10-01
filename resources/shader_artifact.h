#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace NResources {

enum class EShaderStage {
    VERTEX,
    FRAGMENT,
    COMPUTE,
};

class ShaderArtifact final {
public:
    ShaderArtifact() = default;

    ShaderArtifact(EShaderStage stage, std::vector<std::uint32_t> words)
        : m_stage(stage)
        , m_words(std::move(words)) {
    }

    [[nodiscard]] EShaderStage GetStage() const noexcept {
        return m_stage;
    }

    [[nodiscard]] const std::vector<std::uint32_t>& GetWords() const noexcept {
        return m_words;
    }

private:
    EShaderStage m_stage = EShaderStage::VERTEX;
    std::vector<std::uint32_t> m_words;
};

} // namespace NResources
