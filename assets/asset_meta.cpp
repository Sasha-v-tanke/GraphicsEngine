#include "asset_meta.h"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

namespace NAssets {
namespace {

[[nodiscard]] std::expected<std::pair<std::string, std::string>, std::string> ParseLine(const std::string& line) {
    const auto separator = line.find('=');
    if (separator == std::string::npos || separator == 0 || separator + 1 == line.size()) {
        return std::unexpected("metadata lines must be non-empty key=value pairs");
    }
    return std::pair{line.substr(0, separator), line.substr(separator + 1)};
}

[[nodiscard]] bool IsSingleLineField(const std::string& value) {
    return !value.empty() && value.find_first_of("\r\n") == std::string::npos;
}

[[nodiscard]] bool IsSettingKey(const std::string& value) {
    return IsSingleLineField(value) && value.find('=') == std::string::npos;
}

class TemporaryFile final {
public:
    explicit TemporaryFile(const std::string& path) noexcept
        : Path(path) {
        Descriptor = ::open(Path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    }
    ~TemporaryFile() {
        if (Descriptor >= 0)
            ::close(Descriptor);
        if (Created)
            ::unlink(Path.c_str());
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    const std::string& Path;
    int Descriptor = -1;
    bool Created = false;
};

[[nodiscard]] std::expected<void, std::string> WriteAll(int descriptor, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = ::write(descriptor, data.data() + offset, data.size() - offset);
        if (written <= 0)
            return std::unexpected("cannot write asset metadata");
        offset += static_cast<std::size_t>(written);
    }
    return {};
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
    std::size_t bytes = 0;
    while (input.peek() != std::char_traits<char>::eof()) {
        line.clear();
        char character;
        while (input.get(character)) {
            if (++bytes > MaxMetaBytes)
                return std::unexpected("metadata exceeds byte limit");
            if (character == '\n')
                break;
            if (line.size() == MaxMetaLineBytes)
                return std::unexpected("metadata exceeds line limit");
            line.push_back(character);
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto parsed = ParseLine(line);
        if (!parsed)
            return std::unexpected(parsed.error());
        const auto& [key, value] = *parsed;
        if (!IsSingleLineField(value) || !IsSingleLineField(key))
            return std::unexpected("metadata fields must be non-empty single-line fields");
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
            if (result.Settings.size() == MaxMetaSettings || !IsSettingKey(settingKey) ||
                !result.Settings.emplace(settingKey, value).second)
                return std::unexpected("duplicate or empty setting key");
        } else
            return std::unexpected("unknown metadata key");
    }
    if (input.bad() || (!input.eof() && input.fail()))
        return std::unexpected("cannot read asset metadata");
    if (!idSeen || !importerSeen || !schemaSeen || result.Importer.empty())
        return std::unexpected("missing required asset metadata field");
    return result;
}

std::expected<void, std::string> CreateMeta(const std::filesystem::path& source, const AssetMeta& meta) {
    if (!IsSingleLineField(meta.Importer) || meta.SchemaVersion == 0)
        return std::unexpected("metadata importer and schema version are required");
    if (meta.Settings.size() > MaxMetaSettings || meta.Importer.size() > MaxMetaLineBytes - 9)
        return std::unexpected("metadata exceeds field limits");
    for (const auto& [key, value]: meta.Settings) {
        if (!IsSettingKey(key) || !IsSingleLineField(value))
            return std::unexpected("metadata settings must be non-empty single-line key=value fields");
        if (key.size() > MaxMetaLineBytes - 9 || value.size() > MaxMetaLineBytes - 9 - key.size())
            return std::unexpected("metadata exceeds line limit");
    }
    std::string serialized = "asset_id=" + meta.Id.ToString() + "\nimporter=" + meta.Importer +
                             "\nschema_version=" + std::to_string(meta.SchemaVersion) + "\n";
    for (const auto& [key, value]: meta.Settings) {
        if (9 + key.size() + value.size() + 1 > MaxMetaBytes - serialized.size())
            return std::unexpected("metadata exceeds byte limit");
        serialized += "setting." + key + '=' + value + '\n';
    }
    const auto target = MetaPathFor(source);
    static std::atomic_uint64_t sequence{};
    const auto temporary =
            target.string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(sequence.fetch_add(1));
    TemporaryFile file(temporary);
    if (file.Descriptor < 0)
        return std::unexpected("cannot create unique asset metadata temporary file");
    file.Created = true;
    if (const auto write = WriteAll(file.Descriptor, serialized); !write || ::fsync(file.Descriptor) != 0) {
        return std::unexpected(write ? "cannot sync asset metadata" : write.error());
    }
    const int closeResult = ::close(file.Descriptor);
    file.Descriptor = -1;
    if (closeResult != 0) {
        return std::unexpected("cannot close asset metadata");
    }
    if (::link(temporary.c_str(), target.c_str()) != 0) {
        const int publishError = errno;
        if (publishError == EEXIST)
            return std::unexpected("asset metadata already exists");
        return std::unexpected("cannot publish asset metadata");
    }
    return {};
}

} // namespace NAssets
