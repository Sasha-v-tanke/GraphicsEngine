#include "shader_artifact_loader.h"

#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace NResources {

namespace {

constexpr std::uint32_t SPIR_V_MAGIC = 0x07230203;
constexpr std::size_t SPIR_V_HEADER_WORDS = 5;

[[nodiscard]] std::string
MakeShaderErrorMessage(const ResourceIdentity& identity, const std::filesystem::path& path, const std::string& reason) {
    return "Shader artifact '" + std::string{identity.GetKey()} + "' at '" + path.string() + "': " + reason;
}

[[nodiscard]] std::vector<std::uint32_t> ReadShaderWords(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};

    if (!file) {
        return {};
    }

    const std::vector<char> bytes{
            std::istreambuf_iterator<char>{file},
            std::istreambuf_iterator<char>{},
    };

    if (bytes.empty() || bytes.size() % sizeof(std::uint32_t) != 0) {
        return {};
    }

    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());

    return words;
}

void FailShaderLoad(ResourceManager& resources,
                    ResourceOperation<ShaderArtifact> operation,
                    const ResourceIdentity& identity,
                    const std::filesystem::path& path,
                    const std::string& reason) {
    resources.Fail(operation,
                   NCommon::ErrorInfo{
                           .Code = NCommon::EError::IO_ERROR,
                           .Message = MakeShaderErrorMessage(identity, path, reason),
                   });
}

} // namespace

bool IsValidSpirVArtifact(const std::vector<std::uint32_t>& words) noexcept {
    return words.size() >= SPIR_V_HEADER_WORDS && words[0] == SPIR_V_MAGIC && words[1] != 0 && words[3] != 0;
}

ResourceHandle<ShaderArtifact> LoadShaderArtifact(ResourceManager& resources,
                                                  ResourceIdentity identity,
                                                  const std::filesystem::path& path,
                                                  EShaderStage stage) {
    const ResourceIdentity failureIdentity = identity;
    const ResourceHandle<ShaderArtifact> handle = resources.Request<ShaderArtifact>(std::move(identity));
    const ResourceOperation<ShaderArtifact> operation = resources.BeginLoading(handle);
    const std::vector<std::uint32_t> words = ReadShaderWords(path);

    if (words.empty()) {
        FailShaderLoad(resources, operation, failureIdentity, path, "missing or unreadable SPIR-V binary");
        return handle;
    }

    if (!IsValidSpirVArtifact(words)) {
        FailShaderLoad(resources, operation, failureIdentity, path, "invalid SPIR-V artifact");
        return handle;
    }

    resources.PublishReady(operation, std::make_shared<ShaderArtifact>(stage, words));

    return handle;
}

} // namespace NResources
