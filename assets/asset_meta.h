#pragma once

#include "GraphicsEngine/assets/asset_id.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>

namespace NAssets {

struct AssetMeta final {
    AssetId Id;
    std::string Importer;
    std::uint32_t SchemaVersion = 1;
    std::map<std::string, std::string, std::less<>> Settings;

    [[nodiscard]] friend bool operator==(const AssetMeta&, const AssetMeta&) = default;
};

[[nodiscard]] std::filesystem::path MetaPathFor(const std::filesystem::path& source);
[[nodiscard]] std::expected<AssetMeta, std::string> ReadMeta(const std::filesystem::path& source);
[[nodiscard]] std::expected<void, std::string> CreateMeta(const std::filesystem::path& source, const AssetMeta& meta);

} // namespace NAssets
