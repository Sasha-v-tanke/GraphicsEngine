#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <resources/image_loader.h>
#include <resources/image_loader_internal.h>
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

using NResources::EImagePixelFormat;
using NResources::EResourceState;
using NResources::EShaderStage;
using NResources::ImageData;
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

void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};

    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t>
MakeTga(std::uint16_t width, std::uint16_t height, std::uint8_t imageType, std::uint8_t bitsPerPixel) {
    return {
            0U,
            0U,
            imageType,
            0U,
            0U,
            0U,
            0U,
            0U,
            0U,
            0U,
            0U,
            0U,
            static_cast<std::uint8_t>(width & 0xFFU),
            static_cast<std::uint8_t>(width >> 8U),
            static_cast<std::uint8_t>(height & 0xFFU),
            static_cast<std::uint8_t>(height >> 8U),
            bitsPerPixel,
            0x20U,
    };
}

std::shared_ptr<const ImageData> LoadImageData(const std::filesystem::path& path,
                                               const std::vector<std::uint8_t>& bytes) {
    WriteBytes(path, bytes);

    ResourceManager resources;

    const auto image = NResources::LoadImage(resources, ResourceIdentity{"image", path.filename().string()}, path);
    std::shared_ptr<const ImageData> data = resources.GetCpuResource(image);

    EXPECT_EQ(resources.GetState(image), EResourceState::READY);
    std::filesystem::remove(path);

    return data;
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

TEST(ResourceManager, CoalescesNormalizedPathAliases) {
    ResourceManager resources;

    const auto first = resources.Request<TestShaderArtifact>(
            ResourceIdentity::FromPath("shader", std::filesystem::path{"shaders"} / "basic.vert.spv"));
    const auto second = resources.Request<TestShaderArtifact>(
            ResourceIdentity::FromPath("shader",
                                       std::filesystem::path{"shaders"} / "." / "variants" / ".." / "basic.vert.spv"));

    EXPECT_EQ(first, second);
    EXPECT_EQ(resources.GetIdentity(first).GetKey(), std::string_view{"shaders/basic.vert.spv"});
}

TEST(ResourceManager, CoalescesConcurrentDuplicateRequests) {
    constexpr std::size_t REQUEST_COUNT = 8;

    ResourceManager resources;
    std::array<NResources::ResourceHandle<TestShaderArtifact>, REQUEST_COUNT> handles;
    std::array<std::thread, REQUEST_COUNT> threads;

    for (std::size_t index = 0; index < REQUEST_COUNT; ++index) {
        threads[index] = std::thread{[&, index] {
            handles[index] =
                    resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "concurrent-basic.vert.spv"});
        }};
    }

    for (std::thread& thread: threads) {
        thread.join();
    }

    for (std::size_t index = 1; index < REQUEST_COUNT; ++index) {
        EXPECT_EQ(handles[0], handles[index]);
    }
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

TEST(ResourceManager, SupportsConcurrentStateObservation) {
    ResourceManager resources;

    const auto shader = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "observed.vert.spv"});

    std::thread observer{[&] {
        for (int iteration = 0; iteration < 256; ++iteration) {
            static_cast<void>(resources.GetState(shader));
            static_cast<void>(resources.GetIdentity(shader));
            static_cast<void>(resources.GetCpuResource(shader));
        }
    }};

    for (int iteration = 0; iteration < 256; ++iteration) {
        const auto operation = resources.BeginLoading(shader);
        resources.PublishReady(operation, std::make_shared<TestShaderArtifact>());
        resources.RequestUnload(shader);
        resources.CompleteUnload(shader);
    }

    observer.join();

    EXPECT_EQ(resources.GetState(shader), EResourceState::UNLOADED);
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
    EXPECT_FALSE(resources.IsCancellationRequested(firstOperation));
    EXPECT_FALSE(resources.IsCancellationRequested(retryOperation));

    ExpectError(NCommon::EError::INVALID_STATE,
                [&] { resources.PublishReady(firstOperation, std::make_shared<TestShaderArtifact>()); });

    resources.PublishReady(retryOperation, std::make_shared<TestShaderArtifact>());

    EXPECT_EQ(resources.GetState(shader), EResourceState::READY);
    EXPECT_FALSE(resources.GetFailure(shader).has_value());
}

TEST(ResourceManager, UnloadRequestsCancellationForInFlightOperation) {
    ResourceManager resources;

    const auto shader = resources.Request<TestShaderArtifact>(ResourceIdentity{"shader", "basic.vert.spv"});
    const auto operation = resources.BeginLoading(shader);

    EXPECT_FALSE(resources.IsCancellationRequested(operation));

    resources.RequestUnload(shader);

    EXPECT_EQ(resources.GetState(shader), EResourceState::UNLOADING);
    EXPECT_TRUE(resources.IsCancellationRequested(operation));

    ExpectError(NCommon::EError::INVALID_STATE,
                [&] { resources.PublishReady(operation, std::make_shared<TestShaderArtifact>()); });

    resources.CompleteUnload(shader);

    EXPECT_EQ(resources.GetState(shader), EResourceState::UNLOADED);
    EXPECT_TRUE(resources.IsCancellationRequested(operation));

    const auto nextOperation = resources.BeginLoading(shader);

    EXPECT_FALSE(resources.IsCancellationRequested(nextOperation));
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

TEST(ImageLoader, LoadsValidRgbImageAsRgba8) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-valid-image.ppm");
    const std::vector<std::uint8_t> bytes = {
            'P',
            '6',
            '\n',
            '2',
            '\n',
            '1',
            '\n',
            '2',
            '5',
            '5',
            '\n',
            255U,
            0U,
            0U,
            0U,
            255U,
            0U,
    };
    WriteBytes(path, bytes);

    ResourceManager resources;

    const auto image = NResources::LoadImage(resources, ResourceIdentity{"image", "valid"}, path);
    const std::shared_ptr<const ImageData> data = resources.GetCpuResource(image);

    ASSERT_EQ(resources.GetState(image), EResourceState::READY);
    ASSERT_TRUE(data);
    EXPECT_EQ(data->GetWidth(), 2U);
    EXPECT_EQ(data->GetHeight(), 1U);
    EXPECT_EQ(data->GetFormat(), EImagePixelFormat::RGBA8);
    EXPECT_EQ(data->GetPixels(),
              (std::vector<std::uint8_t>{
                      255U,
                      0U,
                      0U,
                      255U,
                      0U,
                      255U,
                      0U,
                      255U,
              }));

    std::filesystem::remove(path);
}

TEST(ImageLoader, ConvertsGrayscaleImageToRgba8) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-grayscale-image.tga");
    std::vector<std::uint8_t> bytes = MakeTga(2, 1, 3, 8);
    bytes.insert(bytes.end(), {0x22U, 0xCCU});

    const std::shared_ptr<const ImageData> data = LoadImageData(path, bytes);

    ASSERT_TRUE(data);
    EXPECT_EQ(data->GetPixels(),
              (std::vector<std::uint8_t>{
                      0x22U,
                      0x22U,
                      0x22U,
                      255U,
                      0xCCU,
                      0xCCU,
                      0xCCU,
                      255U,
              }));
}

TEST(ImageLoader, PreservesGrayscaleAlphaImageAsRgba8) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-grayscale-alpha-image.tga");
    std::vector<std::uint8_t> bytes = MakeTga(2, 1, 3, 16);
    bytes.insert(bytes.end(), {0x33U, 0x44U, 0xAAU, 0xBBU});

    const std::shared_ptr<const ImageData> data = LoadImageData(path, bytes);

    ASSERT_TRUE(data);
    EXPECT_EQ(data->GetPixels(),
              (std::vector<std::uint8_t>{
                      0x33U,
                      0x33U,
                      0x33U,
                      0x44U,
                      0xAAU,
                      0xAAU,
                      0xAAU,
                      0xBBU,
              }));
}

TEST(ImageLoader, ConvertsRgbImageToRgba8WithoutVerticalFlip) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-rgb-two-row-image.tga");
    std::vector<std::uint8_t> bytes = MakeTga(1, 2, 2, 24);
    bytes.insert(bytes.end(), {0U, 0U, 255U, 0U, 255U, 0U});

    const std::shared_ptr<const ImageData> data = LoadImageData(path, bytes);

    ASSERT_TRUE(data);
    EXPECT_EQ(data->GetWidth(), 1U);
    EXPECT_EQ(data->GetHeight(), 2U);
    EXPECT_EQ(data->GetFormat(), EImagePixelFormat::RGBA8);
    EXPECT_EQ(data->GetPixels(),
              (std::vector<std::uint8_t>{
                      255U,
                      0U,
                      0U,
                      255U,
                      0U,
                      255U,
                      0U,
                      255U,
              }));
}

TEST(ImageLoader, PreservesRgbaImageAsRgba8) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-rgba-image.tga");
    std::vector<std::uint8_t> bytes = MakeTga(2, 1, 2, 32);
    bytes.insert(bytes.end(), {0U, 0U, 255U, 17U, 0U, 255U, 0U, 221U});

    const std::shared_ptr<const ImageData> data = LoadImageData(path, bytes);

    ASSERT_TRUE(data);
    EXPECT_EQ(data->GetPixels(),
              (std::vector<std::uint8_t>{
                      255U,
                      0U,
                      0U,
                      17U,
                      0U,
                      255U,
                      0U,
                      221U,
              }));
}

TEST(ImageLoader, FailsCorruptImageData) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-corrupt-image.bin");
    WriteBytes(path, {'n', 'o', 't', 'i', 'm', 'a', 'g', 'e'});

    ResourceManager resources;

    const auto image = NResources::LoadImage(resources, ResourceIdentity{"image", "corrupt"}, path);
    const std::optional<NCommon::ErrorInfo> failure = resources.GetFailure(image);

    ASSERT_EQ(resources.GetState(image), EResourceState::FAILED);
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->Code, NCommon::make_error_code(NCommon::EError::IO_ERROR));
    EXPECT_NE(failure->Message.find("invalid or corrupt image data"), std::string::npos);
    EXPECT_FALSE(resources.GetCpuResource(image));

    std::filesystem::remove(path);
}

TEST(ImageLoader, FailsMissingImageData) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-missing-image.ppm");
    std::filesystem::remove(path);

    ResourceManager resources;

    const auto image = NResources::LoadImage(resources, ResourceIdentity{"image", "missing"}, path);
    const std::optional<NCommon::ErrorInfo> failure = resources.GetFailure(image);

    ASSERT_EQ(resources.GetState(image), EResourceState::FAILED);
    ASSERT_TRUE(failure.has_value());
    EXPECT_NE(failure->Message.find("missing or unreadable image data"), std::string::npos);
    EXPECT_FALSE(resources.GetCpuResource(image));
}

TEST(ImageLoader, RejectsOversizedImageBeforeReading) {
    const std::filesystem::path path = MakeTempArtifactPath("graphics-engine-oversized-image.bin");
    WriteBytes(path, {});
    std::filesystem::resize_file(path, static_cast<std::uintmax_t>(std::numeric_limits<int>::max()) + 1U);

    ResourceManager resources;

    const auto image = NResources::LoadImage(resources, ResourceIdentity{"image", "oversized"}, path);
    const std::optional<NCommon::ErrorInfo> failure = resources.GetFailure(image);

    ASSERT_EQ(resources.GetState(image), EResourceState::FAILED);
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->Code, NCommon::make_error_code(NCommon::EError::IO_ERROR));
    EXPECT_NE(failure->Message.find("image data exceeds stb_image size limit"), std::string::npos);
    EXPECT_FALSE(resources.GetCpuResource(image));

    std::filesystem::remove(path);
}

TEST(ImageLoader, RejectsOversizedReadBufferBeforeStbCast) {
    EXPECT_TRUE(NResources::NImageLoaderInternal::CanPassImageByteSizeToStb(
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
    EXPECT_FALSE(NResources::NImageLoaderInternal::CanPassImageByteSizeToStb(
            static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1U));
}

} // namespace
