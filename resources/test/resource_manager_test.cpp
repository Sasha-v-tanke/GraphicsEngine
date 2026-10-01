#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <resources/resource_manager.h>
#include <resources/shader_artifact_loader.h>
#include <tests/common/test_error.h>

namespace {

struct TestShaderArtifact {
    std::string Name;
};

struct TextureArtifact {
    std::string Name;
};

using NResources::EResourceState;
using NResources::EShaderStage;
using NResources::ResourceIdentity;
using NResources::ResourceManager;
using NResources::ShaderArtifact;
using NTest::ExpectError;

std::filesystem::path MakeTempArtifactPath(std::string_view name) {
    return std::filesystem::temp_directory_path() / std::string{name};
}

void WriteWords(const std::filesystem::path& path, const std::vector<std::uint32_t>& words) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};

    file.write(reinterpret_cast<const char*>(words.data()),
               static_cast<std::streamsize>(words.size() * sizeof(std::uint32_t)));
}

TEST(ResourceManager, CreatesTypedHandleForLogicalIdentity) {
    ResourceManager resources;

    const auto shader = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});

    ASSERT_TRUE(shader.IsValid());
    EXPECT_EQ(resources.GetState(shader), EResourceState::UNLOADED);
    EXPECT_EQ(resources.GetIdentity(shader).GetResourceClass(), std::string_view{"shader"});
    EXPECT_EQ(resources.GetIdentity(shader).GetKey(), std::string_view{"basic.vert.spv"});
}

TEST(ResourceManager, CoalescesDuplicateIdentity) {
    ResourceManager resources;

    const auto first = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});
    const auto second = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});

    EXPECT_EQ(first, second);
}

TEST(ResourceManager, ReturnsIdentityByStableValue) {
    ResourceManager resources;

    const auto shader = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});
    const ResourceIdentity identity = resources.GetIdentity(shader);

    for (int index = 0; index < 128; ++index) {
        static_cast<void>(
                resources.Request<TextureArtifact>(ResourceIdentity{"texture", "texture-" + std::to_string(index)}));
    }

    EXPECT_EQ(identity.GetResourceClass(), std::string_view{"shader"});
    EXPECT_EQ(identity.GetKey(), std::string_view{"basic.vert.spv"});
}

TEST(ResourceManager, RejectsSameIdentityWithDifferentType) {
    ResourceManager resources;

    static_cast<void>(resources.Request<TestShaderArtifact>(ResourceIdentity{"asset", "shared"}));

    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { static_cast<void>(resources.Request<TextureArtifact>(ResourceIdentity{"asset", "shared"})); });
}

TEST(ResourceManager, PublishesCpuResourceAfterLoading) {
    ResourceManager resources;

    const auto shader = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});
    const auto operation = resources.BeginLoading(shader);

    resources.PublishReady(operation, std::make_shared<TestShaderArtifact>(TestShaderArtifact{.Name = "basic"}));

    ASSERT_EQ(resources.GetState(shader), EResourceState::READY);
    ASSERT_TRUE(resources.GetCpuResource(shader));
    EXPECT_EQ(resources.GetCpuResource(shader)->Name, "basic");
}

TEST(ResourceManager, ValidatesLegalTransitions) {
    ResourceManager resources;

    const auto shader = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});

    ExpectError(NCommon::EError::INVALID_STATE, [&] { resources.CompleteUnload(shader); });

    const auto operation = resources.BeginLoading(shader);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(resources.BeginLoading(shader)); });

    resources.PublishReady(operation, std::make_shared<TestShaderArtifact>());

    ExpectError(NCommon::EError::INVALID_STATE,
                [&] { resources.PublishReady(operation, std::make_shared<TestShaderArtifact>()); });

    resources.RequestUnload(shader);
    resources.CompleteUnload(shader);

    EXPECT_EQ(resources.GetState(shader), EResourceState::UNLOADED);
}

TEST(ResourceManager, RetryInvalidatesStaleOperation) {
    ResourceManager resources;

    const auto shader = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});
    const auto firstOperation = resources.BeginLoading(shader);

    resources.Fail(firstOperation,
                   NCommon::ErrorInfo{
                           .Code = NCommon::EError::IO_ERROR,
                           .Message = "read failed",
                   });

    const std::optional<NCommon::ErrorInfo> failure = resources.GetFailure(shader);

    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->Code, NCommon::make_error_code(NCommon::EError::IO_ERROR));
    EXPECT_EQ(failure->Message, "read failed");

    const auto retryOperation = resources.BeginLoading(shader);

    EXPECT_NE(firstOperation.GetGeneration(), retryOperation.GetGeneration());

    ExpectError(NCommon::EError::INVALID_STATE,
                [&] { resources.PublishReady(firstOperation, std::make_shared<TestShaderArtifact>()); });

    resources.PublishReady(retryOperation, std::make_shared<TestShaderArtifact>());

    EXPECT_EQ(resources.GetState(shader), EResourceState::READY);
    EXPECT_FALSE(resources.GetFailure(shader).has_value());
}

TEST(ResourceManager, UnloadInvalidatesInFlightOperation) {
    ResourceManager resources;

    const auto shader = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});
    const auto operation = resources.BeginLoading(shader);

    resources.RequestUnload(shader);

    ExpectError(NCommon::EError::INVALID_STATE,
                [&] { resources.PublishReady(operation, std::make_shared<TestShaderArtifact>()); });

    resources.CompleteUnload(shader);

    EXPECT_EQ(resources.GetState(shader), EResourceState::UNLOADED);
}

TEST(ResourceManager, RejectsStaleHandleAfterForget) {
    ResourceManager resources;

    const auto first = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});

    resources.Forget(first);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(resources.GetState(first)); });

    const auto second = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});

    EXPECT_TRUE(second.IsValid());
    EXPECT_NE(first.GetGeneration(), second.GetGeneration());
}

TEST(ShaderArtifactLoader, LoadsValidSpirVArtifact) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-valid-shader.spv");
    WriteWords(path, {0x07230203, 0x00010000, 0, 1, 0});

    ResourceManager resources;

    const auto shader =
            NResources::LoadShaderArtifact(resources, ResourceIdentity{"shader", "valid"}, path, EShaderStage::VERTEX);
    const std::shared_ptr<const ShaderArtifact> artifact = resources.GetCpuResource(shader);

    ASSERT_EQ(resources.GetState(shader), EResourceState::READY);
    ASSERT_TRUE(artifact);
    EXPECT_EQ(artifact->GetStage(), EShaderStage::VERTEX);
    EXPECT_EQ(artifact->GetWords().front(), 0x07230203U);

    std::filesystem::remove(path);
}

TEST(ShaderArtifactLoader, FailsInvalidArtifact) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-invalid-shader.spv");
    WriteWords(path, {0xDEADBEEF, 0x00010000, 0, 1, 0});

    ResourceManager resources;

    const auto shader = NResources::LoadShaderArtifact(resources,
                                                       ResourceIdentity{"shader", "invalid"},
                                                       path,
                                                       EShaderStage::FRAGMENT);
    const std::optional<NCommon::ErrorInfo> failure = resources.GetFailure(shader);

    ASSERT_EQ(resources.GetState(shader), EResourceState::FAILED);
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->Code, NCommon::make_error_code(NCommon::EError::IO_ERROR));
    EXPECT_NE(failure->Message.find("invalid SPIR-V artifact"), std::string::npos);
    EXPECT_FALSE(resources.GetCpuResource(shader));

    std::filesystem::remove(path);
}

TEST(ShaderArtifactLoader, RejectsInvalidVersionEncoding) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-invalid-version-shader.spv");
    WriteWords(path, {0x07230203, 0xFFFFFFFF, 0, 1, 0});

    ResourceManager resources;

    const auto shader = NResources::LoadShaderArtifact(resources,
                                                       ResourceIdentity{"shader", "invalid-version"},
                                                       path,
                                                       EShaderStage::FRAGMENT);

    EXPECT_EQ(resources.GetState(shader), EResourceState::FAILED);
    EXPECT_FALSE(resources.GetCpuResource(shader));

    std::filesystem::remove(path);
}

TEST(ShaderArtifactLoader, RejectsNonZeroSchema) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-non-zero-schema-shader.spv");
    WriteWords(path, {0x07230203, 0x00010000, 0, 1, 1});

    ResourceManager resources;

    const auto shader = NResources::LoadShaderArtifact(resources,
                                                       ResourceIdentity{"shader", "non-zero-schema"},
                                                       path,
                                                       EShaderStage::FRAGMENT);

    EXPECT_EQ(resources.GetState(shader), EResourceState::FAILED);
    EXPECT_FALSE(resources.GetCpuResource(shader));

    std::filesystem::remove(path);
}

TEST(ShaderArtifactLoader, FailsMissingArtifact) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-missing-shader.spv");
    std::filesystem::remove(path);

    ResourceManager resources;

    const auto shader = NResources::LoadShaderArtifact(resources,
                                                       ResourceIdentity{"shader", "missing"},
                                                       path,
                                                       EShaderStage::COMPUTE);
    const std::optional<NCommon::ErrorInfo> failure = resources.GetFailure(shader);

    ASSERT_EQ(resources.GetState(shader), EResourceState::FAILED);
    ASSERT_TRUE(failure.has_value());
    EXPECT_NE(failure->Message.find("missing or unreadable SPIR-V binary"), std::string::npos);
    EXPECT_FALSE(resources.GetCpuResource(shader));
}

} // namespace
