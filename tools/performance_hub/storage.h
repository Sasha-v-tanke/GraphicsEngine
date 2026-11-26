#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace graphics_engine::performance_hub {

enum class ErrorCode {
    invalid_argument,
    not_found,
    conflict,
    io,
    database,
    migration
};
struct Error {
    ErrorCode code;
    std::string message;
};
template<typename T>
using Result = std::expected<T, Error>;

enum class RunStatus {
    staged,
    completed,
    failed
};
struct RunDraft {
    std::string source;
    std::string job_id;
    std::string git_revision;
    std::string environment_fingerprint;
    std::string build_fingerprint;
    std::string run_config_json;
};
struct Run {
    std::string id;
    RunStatus status;
    std::string source;
    std::string started_at_utc;
    std::optional<std::string> ended_at_utc;
    std::string run_config_json;
};
struct ArtifactInput {
    std::filesystem::path source_file;
    std::filesystem::path relative_path;
    std::string type;
};
struct Artifact {
    std::string content_hash;
    std::uintmax_t size;
    std::string type;
    std::filesystem::path relative_path;
};
struct RunPage {
    std::vector<Run> runs;
    std::optional<std::string> next_cursor;
};

class Storage final {
public:
    static Result<Storage> Open(const std::filesystem::path& root);
    Storage(Storage&&) noexcept;
    Storage& operator=(Storage&&) noexcept;
    ~Storage();
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    Result<Run> CreateStage(const RunDraft& draft);
    Result<void> Complete(const std::string& run_id, const std::vector<ArtifactInput>& artifacts);
    Result<void> Fail(const std::string& run_id, std::string_view reason);
    Result<Run> Read(std::string_view run_id) const;
    Result<RunPage> List(std::size_t limit, std::optional<std::string_view> after_id = std::nullopt) const;
    [[nodiscard]] const std::filesystem::path& root() const noexcept;

private:
    struct Impl;
    explicit Storage(Impl* impl) noexcept;
    Impl* impl_ = nullptr;
};

} // namespace graphics_engine::performance_hub
