#include "asset_meta.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace NAssets {
namespace {

[[nodiscard]] std::expected<std::pair<std::string, std::string>, std::string> ParseLine(const std::string& line) {
    const auto separator = line.find('=');
    if (separator == std::string::npos || separator == 0 || separator + 1 == line.size()) {
        return std::unexpected("metadata lines must be non-empty key=value pairs");
    }
    return std::pair{line.substr(0, separator), line.substr(separator + 1)};
}

} // namespace

std::filesystem::path MetaPathFor(const std::filesystem::path& source) {
    return source.string() + ".meta";
}

std::expected<AssetMeta, std::string> ReadMeta(const std::filesystem::path& source) {
    std::ifstream input(MetaPathFor(source), std::ios::binary);
    if (!input)
        return std::unexpected("cannot open asset metadata");

    AssetMeta result;
    bool idSeen = false, importerSeen = false, schemaSeen = false;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto parsed = ParseLine(line);
        if (!parsed)
            return std::unexpected(parsed.error());
        const auto& [key, value] = *parsed;
        if (key == "asset_id") {
            if (idSeen)
                return std::unexpected("duplicate asset_id");
            const auto id = AssetId::Parse(value);
            if (!id)
                return std::unexpected(id.error());
            result.Id = *id;
            idSeen = true;
        } else if (key == "importer") {
            if (importerSeen)
                return std::unexpected("duplicate importer");
            result.Importer = value;
            importerSeen = true;
        } else if (key == "schema_version") {
            if (schemaSeen)
                return std::unexpected("duplicate schema_version");
            const auto [pointer, error] =
                    std::from_chars(value.data(), value.data() + value.size(), result.SchemaVersion);
            if (error != std::errc{} || pointer != value.data() + value.size() || result.SchemaVersion == 0)
                return std::unexpected("invalid schema_version");
            schemaSeen = true;
        } else if (key.starts_with("setting.")) {
            const std::string settingKey = key.substr(8);
            if (settingKey.empty() || !result.Settings.emplace(settingKey, value).second)
                return std::unexpected("duplicate or empty setting key");
        } else
            return std::unexpected("unknown metadata key");
    }
    if (!input.eof())
        return std::unexpected("cannot read asset metadata");
    if (!idSeen || !importerSeen || !schemaSeen || result.Importer.empty())
        return std::unexpected("missing required asset metadata field");
    return result;
}

std::expected<void, std::string> CreateMeta(const std::filesystem::path& source, const AssetMeta& meta) {
    if (meta.Importer.empty() || meta.SchemaVersion == 0)
        return std::unexpected("metadata importer and schema version are required");
    const auto target = MetaPathFor(source);
    if (std::filesystem::exists(target))
        return std::unexpected("asset metadata already exists");
    const auto temporary = target.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            return std::unexpected("cannot create asset metadata");
        output << "asset_id=" << meta.Id.ToString() << '\n'
               << "importer=" << meta.Importer << '\n'
               << "schema_version=" << meta.SchemaVersion << '\n';
        for (const auto& [key, value]: meta.Settings)
            output << "setting." << key << '=' << value << '\n';
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary);
            return std::unexpected("cannot write asset metadata");
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::filesystem::remove(temporary);
        return std::unexpected("cannot publish asset metadata");
    }
    return {};
}

} // namespace NAssets
