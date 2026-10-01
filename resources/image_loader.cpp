#include "image_loader.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <stb_image.h>
#include <string>
#include <vector>

namespace NResources {

namespace {

constexpr int RGBA_CHANNELS = 4;

struct StbImageDeleter final {
    void operator()(stbi_uc* pixels) const noexcept {
        stbi_image_free(pixels);
    }
};

[[nodiscard]] std::string
MakeImageErrorMessage(const ResourceIdentity& identity, const std::filesystem::path& path, const std::string& reason) {
    return "Image resource '" + std::string{identity.GetKey()} + "' at '" + path.string() + "': " + reason;
}

[[nodiscard]] std::vector<stbi_uc> ReadImageBytes(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};

    if (!file) {
        return {};
    }

    return {
            std::istreambuf_iterator<char>{file},
            std::istreambuf_iterator<char>{},
    };
}

void FailImageLoad(ResourceManager& resources,
                   ResourceOperation<ImageData> operation,
                   const ResourceIdentity& identity,
                   const std::filesystem::path& path,
                   const std::string& reason) {
    resources.Fail(operation,
                   NCommon::ErrorInfo{
                           .Code = NCommon::EError::IO_ERROR,
                           .Message = MakeImageErrorMessage(identity, path, reason),
                   });
}

} // namespace

ResourceHandle<ImageData>
LoadImage(ResourceManager& resources, ResourceIdentity identity, const std::filesystem::path& path) {
    const ResourceIdentity failureIdentity = identity;
    const ResourceHandle<ImageData> handle = resources.Request<ImageData>(std::move(identity));
    const ResourceOperation<ImageData> operation = resources.BeginLoading(handle);
    const std::vector<stbi_uc> bytes = ReadImageBytes(path);

    if (bytes.empty()) {
        FailImageLoad(resources, operation, failureIdentity, path, "missing or unreadable image data");
        return handle;
    }

    int width = 0;
    int height = 0;
    int sourceChannels = 0;
    stbi_set_flip_vertically_on_load(0);
    std::unique_ptr<stbi_uc, StbImageDeleter> pixels{
            stbi_load_from_memory(bytes.data(),
                                  static_cast<int>(bytes.size()),
                                  &width,
                                  &height,
                                  &sourceChannels,
                                  RGBA_CHANNELS),
    };

    if (!pixels || width <= 0 || height <= 0 || sourceChannels <= 0) {
        FailImageLoad(resources, operation, failureIdentity, path, "invalid or corrupt image data");
        return handle;
    }

    const auto pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> pixelBytes(pixelCount * RGBA_CHANNELS);
    std::copy_n(pixels.get(), pixelBytes.size(), pixelBytes.begin());

    resources.PublishReady(operation,
                           std::make_shared<ImageData>(static_cast<std::uint32_t>(width),
                                                       static_cast<std::uint32_t>(height),
                                                       EImagePixelFormat::RGBA8,
                                                       std::move(pixelBytes)));

    return handle;
}

} // namespace NResources
