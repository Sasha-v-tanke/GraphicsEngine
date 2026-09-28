#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>
#include <resources/resource_manager.h>
#include <tests/common/test_error.h>

namespace {

struct ShaderArtifact {
    std::string Name;
};

struct TextureArtifact {
    std::string Name;
};

using NResources::EResourceState;
using NResources::ResourceIdentity;
using NResources::ResourceManager;
using NTest::ExpectError;

TEST(ResourceManager, CreatesTypedHandleForLogicalIdentity) {
    ResourceManager resources;

    const auto shader = resources.Request<ShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});

    ASSERT_TRUE(shader.IsValid());
    EXPECT_EQ(resources.GetState(shader), EResourceState::UNLOADED);
    EXPECT_EQ(resources.GetIdentity(shader).GetResourceClass(), std::string_view{"shader"});
    EXPECT_EQ(resources.GetIdentity(shader).GetKey(), std::string_view{"basic.vert.spv"});
}

TEST(ResourceManager, CoalescesDuplicateIdentity) {
    ResourceManager resources;

    const auto first = resources.Request<ShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});
    const auto second = resources.Request<ShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});

    EXPECT_EQ(first, second);
}

TEST(ResourceManager, RejectsSameIdentityWithDifferentType) {
    ResourceManager resources;

    static_cast<void>(resources.Request<ShaderArtifact>(ResourceIdentity{"asset", "shared"}));

    ExpectError(NCommon::EError::INVALID_ARGUMENT,
                [&] { static_cast<void>(resources.Request<TextureArtifact>(ResourceIdentity{"asset", "shared"})); });
}

TEST(ResourceManager, PublishesCpuResourceAfterLoading) {
    ResourceManager resources;

    const auto shader = resources.Request<ShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});
    const auto operation = resources.BeginLoading(shader);

    resources.PublishReady(operation, std::make_shared<ShaderArtifact>(ShaderArtifact{.Name = "basic"}));

    ASSERT_EQ(resources.GetState(shader), EResourceState::READY);
    ASSERT_TRUE(resources.GetCpuResource(shader));
    EXPECT_EQ(resources.GetCpuResource(shader)->Name, "basic");
}

TEST(ResourceManager, ValidatesLegalTransitions) {
    ResourceManager resources;

    const auto shader = resources.Request<ShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});

    ExpectError(NCommon::EError::INVALID_STATE, [&] { resources.CompleteUnload(shader); });

    const auto operation = resources.BeginLoading(shader);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(resources.BeginLoading(shader)); });

    resources.PublishReady(operation, std::make_shared<ShaderArtifact>());

    ExpectError(NCommon::EError::INVALID_STATE,
                [&] { resources.PublishReady(operation, std::make_shared<ShaderArtifact>()); });

    resources.RequestUnload(shader);
    resources.CompleteUnload(shader);

    EXPECT_EQ(resources.GetState(shader), EResourceState::UNLOADED);
}

TEST(ResourceManager, RetryInvalidatesStaleOperation) {
    ResourceManager resources;

    const auto shader = resources.Request<ShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});
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
                [&] { resources.PublishReady(firstOperation, std::make_shared<ShaderArtifact>()); });

    resources.PublishReady(retryOperation, std::make_shared<ShaderArtifact>());

    EXPECT_EQ(resources.GetState(shader), EResourceState::READY);
    EXPECT_FALSE(resources.GetFailure(shader).has_value());
}

TEST(ResourceManager, UnloadInvalidatesInFlightOperation) {
    ResourceManager resources;

    const auto shader = resources.Request<ShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});
    const auto operation = resources.BeginLoading(shader);

    resources.RequestUnload(shader);

    ExpectError(NCommon::EError::INVALID_STATE,
                [&] { resources.PublishReady(operation, std::make_shared<ShaderArtifact>()); });

    resources.CompleteUnload(shader);

    EXPECT_EQ(resources.GetState(shader), EResourceState::UNLOADED);
}

TEST(ResourceManager, RejectsStaleHandleAfterForget) {
    ResourceManager resources;

    const auto first = resources.Request<ShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});

    resources.Forget(first);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(resources.GetState(first)); });

    const auto second = resources.Request<ShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});

    EXPECT_TRUE(second.IsValid());
    EXPECT_NE(first.GetGeneration(), second.GetGeneration());
}

} // namespace
