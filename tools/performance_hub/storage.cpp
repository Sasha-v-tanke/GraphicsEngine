#include "tools/performance_hub/storage.h"
#include "tools/performance_hub/storage_internal.h"

#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sqlite3.h>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace graphics_engine::performance_hub {
namespace {

constexpr std::uintmax_t kMaxArtifactBytes = 64U * 1024U * 1024U;
constexpr int kSchemaVersion = 1;

Error MakeError(const ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

std::string NowUtc() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    static std::mutex utc_mutex;
    std::lock_guard lock(utc_mutex);
    const std::tm* utc_source = std::gmtime(&time);
    if (utc_source == nullptr) {
        return {};
    }
    const std::tm utc = *utc_source;
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string NewId() {
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (auto& byte: bytes) {
        byte = static_cast<unsigned char>(random());
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3FU) | 0x80U);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
        if (index == 3U || index == 5U || index == 7U || index == 9U) {
            output << '-';
        }
    }
    return output.str();
}

bool IsSafeRelative(const std::filesystem::path& value) {
    if (value.empty() || value.is_absolute()) {
        return false;
    }
    for (const auto& component: value) {
        if (component == ".." || component == ".") {
            return false;
        }
    }
    return true;
}

std::filesystem::path CanonicalRelative(const std::filesystem::path& value) {
    return value.lexically_normal();
}

Result<void> Sql(sqlite3* database, const int result, std::string_view operation) {
    if (result == SQLITE_OK || result == SQLITE_DONE || result == SQLITE_ROW) {
        return {};
    }
    return std::unexpected(MakeError(ErrorCode::database, std::string(operation) + ": " + sqlite3_errmsg(database)));
}

class Statement final {
public:
    Statement(sqlite3* database, const char* sql)
        : database_(database) {
        result_ = sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr);
    }
    ~Statement() {
        sqlite3_finalize(statement_);
    }
    [[nodiscard]] Result<void> ready() const {
        return Sql(database_, result_, "prepare statement");
    }
    [[nodiscard]] sqlite3_stmt* get() const noexcept {
        return statement_;
    }

private:
    sqlite3* database_;
    sqlite3_stmt* statement_ = nullptr;
    int result_ = SQLITE_ERROR;
};

Result<void> Bind(sqlite3* database, sqlite3_stmt* statement, const int index, std::string_view value) {
    return Sql(database,
               sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT),
               "bind text");
}

RunStatus ToStatus(const unsigned char* value) {
    const std::string_view text(reinterpret_cast<const char*>(value));
    if (text == "completed") {
        return RunStatus::completed;
    }
    if (text == "failed") {
        return RunStatus::failed;
    }
    return RunStatus::staged;
}

Run ReadRow(sqlite3_stmt* statement) {
    Run run{.id = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
            .status = ToStatus(sqlite3_column_text(statement, 1)),
            .source = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
            .started_at_utc = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3)),
            .ended_at_utc = std::nullopt,
            .run_config_json = reinterpret_cast<const char*>(sqlite3_column_text(statement, 5))};
    if (sqlite3_column_type(statement, 4) != SQLITE_NULL) {
        run.ended_at_utc = reinterpret_cast<const char*>(sqlite3_column_text(statement, 4));
    }
    return run;
}

Result<void> ValidateV1Schema(sqlite3* database, const char* schema) {
    sqlite3* reference = nullptr;
    if (sqlite3_open(":memory:", &reference) != SQLITE_OK) {
        sqlite3_close(reference);
        return std::unexpected(MakeError(ErrorCode::migration, "cannot create schema reference"));
    }
    const auto reference_result = sqlite3_exec(reference, schema, nullptr, nullptr, nullptr);
    bool matches = reference_result == SQLITE_OK;
    {
        Statement expected(reference,
                           "SELECT type,name,sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name");
        if (!expected.ready())
            matches = false;
        int step = SQLITE_DONE;
        while (matches && (step = sqlite3_step(expected.get())) == SQLITE_ROW) {
            Statement actual(database, "SELECT sql FROM sqlite_master WHERE type=? AND name=?");
            if (!actual.ready() ||
                !Bind(database,
                      actual.get(),
                      1,
                      reinterpret_cast<const char*>(sqlite3_column_text(expected.get(), 0))) ||
                !Bind(database,
                      actual.get(),
                      2,
                      reinterpret_cast<const char*>(sqlite3_column_text(expected.get(), 1))) ||
                sqlite3_step(actual.get()) != SQLITE_ROW) {
                matches = false;
                break;
            }
            const auto* sql = sqlite3_column_text(actual.get(), 0);
            matches = sql != nullptr && std::string_view(reinterpret_cast<const char*>(sql)) ==
                                                reinterpret_cast<const char*>(sqlite3_column_text(expected.get(), 2));
        }
        if (step != SQLITE_DONE)
            matches = false;
    }
    sqlite3_close(reference);
    if (!matches)
        return std::unexpected(
                MakeError(ErrorCode::migration, "schema constraints or indexes differ from v1 contract"));
    constexpr std::array queries{
            "SELECT "
            "run_id,status,source,started_at_utc,ended_at_utc,job_id,git_revision,environment_fingerprint,build_"
            "fingerprint,run_config_json,failure_reason FROM runs LIMIT 0",
            "SELECT run_id,case_key,outcome FROM run_cases LIMIT 0",
            "SELECT metric_id,group_path,unit,optimization,allowed_aggregates FROM metric_descriptors LIMIT 0",
            "SELECT run_id,case_key,metric_id,repetition,value FROM metric_samples LIMIT 0",
            "SELECT run_id,case_key,metric_id,aggregate_name,value FROM metric_aggregates LIMIT 0",
            "SELECT run_id,relative_path,content_hash,byte_size,type FROM artifacts LIMIT 0",
            "SELECT job_id,metadata_json FROM job_metadata LIMIT 0"};
    for (const char* query: queries) {
        Statement statement(database, query);
        if (auto result = statement.ready(); !result) {
            return std::unexpected(
                    MakeError(ErrorCode::migration, "incompatible v1 schema: " + result.error().message));
        }
    }
    return {};
}

Result<void> RecoverPublishedDirectories(sqlite3* database, const std::filesystem::path& runs_directory) {
    if (auto result =
                Sql(database, sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), "lock recovery");
        !result)
        return std::unexpected(result.error());
    const auto rollback = [&] { sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr); };
    std::error_code error;
    for (const auto& entry: std::filesystem::directory_iterator(runs_directory, error)) {
        if (error)
            break;
        if (!entry.is_directory(error) || error)
            continue;
        Statement status(database, "SELECT status FROM runs WHERE run_id=?");
        if (auto result = status.ready(); !result) {
            rollback();
            return std::unexpected(result.error());
        }
        const auto run_id = entry.path().filename().string();
        if (auto result = Bind(database, status.get(), 1, run_id); !result) {
            rollback();
            return std::unexpected(result.error());
        }
        const int step = sqlite3_step(status.get());
        if (step != SQLITE_ROW && step != SQLITE_DONE) {
            rollback();
            return std::unexpected(
                    MakeError(ErrorCode::database, "read recovery status: " + std::string(sqlite3_errmsg(database))));
        }
        const bool keep =
                step == SQLITE_ROW &&
                std::string_view(reinterpret_cast<const char*>(sqlite3_column_text(status.get(), 0))) == "completed";
        if (!keep) {
            std::filesystem::remove_all(entry.path(), error);
            if (error) {
                rollback();
                return std::unexpected(MakeError(ErrorCode::io, "cannot recover unpublished artifact directory"));
            }
        }
    }
    if (error) {
        rollback();
        return std::unexpected(MakeError(ErrorCode::io, "cannot enumerate published artifact directories"));
    }
    return Sql(database, sqlite3_exec(database, "COMMIT", nullptr, nullptr, nullptr), "commit recovery");
}

Result<void> CopyArtifact(const ArtifactInput& artifact, const std::filesystem::path& staging, Artifact& result) {
    if (!IsSafeRelative(artifact.relative_path) || artifact.type.empty()) {
        return std::unexpected(MakeError(ErrorCode::invalid_argument, "artifact path or type is invalid"));
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(artifact.source_file, error) || error) {
        return std::unexpected(MakeError(ErrorCode::io, "artifact source is not a regular file"));
    }
    const std::uintmax_t size = std::filesystem::file_size(artifact.source_file, error);
    if (error || size > kMaxArtifactBytes) {
        return std::unexpected(MakeError(ErrorCode::io, "artifact is missing or exceeds the storage limit"));
    }
    const auto canonical_path = CanonicalRelative(artifact.relative_path);
    const auto target = staging / canonical_path;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
        return std::unexpected(MakeError(ErrorCode::io, "cannot create artifact directory"));
    }
    std::ifstream input(artifact.source_file, std::ios::binary);
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        return std::unexpected(MakeError(ErrorCode::io, "cannot open artifact file"));
    }
    const auto copied = detail::CopyArtifactBytes(input, output, kMaxArtifactBytes);
    if (!copied)
        return std::unexpected(copied.error());
    output.close();
    if (input.bad() || !input.eof() || !output) {
        return std::unexpected(MakeError(ErrorCode::io, "artifact copy failed"));
    }
    std::ostringstream encoded;
    encoded << "fnv1a64:" << std::hex << copied->hash;
    result = Artifact{.content_hash = encoded.str(),
                      .size = copied->bytes,
                      .type = artifact.type,
                      .relative_path = canonical_path.generic_string()};
    return {};
}

} // namespace

Result<detail::CopiedArtifact>
detail::CopyArtifactBytes(std::istream& input, std::ostream& output, const std::uintmax_t limit) {
    CopiedArtifact copied{0, 14695981039346656037ULL};
    std::array<char, 8192> buffer{};
    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (input.bad() || (input.fail() && !input.eof()))
            return std::unexpected(MakeError(ErrorCode::io, "artifact read failed"));
        if (static_cast<std::uintmax_t>(count) > limit - copied.bytes)
            return std::unexpected(MakeError(ErrorCode::io, "artifact exceeded storage limit"));
        output.write(buffer.data(), count);
        if (!output)
            return std::unexpected(MakeError(ErrorCode::io, "artifact write failed"));
        copied.bytes += static_cast<std::uintmax_t>(count);
        for (std::streamsize index = 0; index < count; ++index)
            copied.hash = (copied.hash ^ static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)])) *
                          1099511628211ULL;
        if (input.eof())
            return copied;
    }
}

struct Storage::Impl {
    sqlite3* database = nullptr;
    std::filesystem::path root;
};
Storage::Storage(Impl* impl) noexcept
    : impl_(impl) {
}
Storage::Storage(Storage&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr)) {
}
Storage& Storage::operator=(Storage&& other) noexcept {
    if (this != &other) {
        if (impl_ != nullptr) {
            sqlite3_close(impl_->database);
            delete impl_;
        }
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}
Storage::~Storage() {
    if (impl_ != nullptr) {
        sqlite3_close(impl_->database);
        delete impl_;
    }
}
const std::filesystem::path& Storage::root() const noexcept {
    return impl_->root;
}

Result<Storage> Storage::Open(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::create_directories(root / "staging", error);
    std::filesystem::create_directories(root / "runs", error);
    if (error) {
        return std::unexpected(MakeError(ErrorCode::io, "cannot create storage directories"));
    }
    auto* impl = new Impl{};
    impl->root = std::filesystem::absolute(root);
    if (sqlite3_open_v2((impl->root / "performance-hub.sqlite3").c_str(),
                        &impl->database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        const std::string message = impl->database == nullptr ? "cannot open database" : sqlite3_errmsg(impl->database);
        sqlite3_close(impl->database);
        delete impl;
        return std::unexpected(MakeError(ErrorCode::database, message));
    }
    if (auto result = Sql(impl->database,
                          sqlite3_exec(impl->database,
                                       "PRAGMA foreign_keys = ON; CREATE TABLE IF NOT EXISTS schema_migrations(version "
                                       "INTEGER PRIMARY KEY, applied_at_utc TEXT NOT NULL)",
                                       nullptr,
                                       nullptr,
                                       nullptr),
                          "initialize migration ledger");
        !result) {
        sqlite3_close(impl->database);
        delete impl;
        return std::unexpected(result.error());
    }
    int version = 0;
    {
        Statement version_query(impl->database, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations");
        if (auto result = version_query.ready(); !result || sqlite3_step(version_query.get()) != SQLITE_ROW) {
            version = -1;
        } else
            version = sqlite3_column_int(version_query.get(), 0);
    }
    if (version < 0) {
        sqlite3_close(impl->database);
        delete impl;
        return std::unexpected(MakeError(ErrorCode::migration, "cannot read schema version"));
    }
    if (version > kSchemaVersion) {
        sqlite3_close(impl->database);
        delete impl;
        return std::unexpected(MakeError(ErrorCode::migration, "database schema is newer than this binary"));
    }
    const char* migration_v1 =
            "CREATE TABLE IF NOT EXISTS runs(run_id TEXT PRIMARY KEY, "
            "status TEXT NOT NULL CHECK(status IN ('staged','completed','failed')), source TEXT NOT NULL, "
            "started_at_utc TEXT NOT NULL, ended_at_utc TEXT, job_id TEXT NOT NULL, git_revision TEXT NOT NULL, "
            "environment_fingerprint TEXT NOT NULL, build_fingerprint TEXT NOT NULL, run_config_json TEXT NOT NULL, "
            "failure_reason TEXT); CREATE TABLE IF NOT EXISTS run_cases(run_id TEXT NOT NULL REFERENCES runs(run_id), "
            "case_key TEXT NOT NULL, outcome TEXT NOT NULL, PRIMARY KEY(run_id, case_key)); CREATE TABLE IF NOT EXISTS "
            "metric_descriptors(metric_id TEXT PRIMARY KEY, group_path TEXT NOT NULL, unit TEXT NOT NULL, optimization "
            "TEXT NOT NULL CHECK(optimization IN ('minimize','maximize','neutral')), allowed_aggregates TEXT NOT "
            "NULL); CREATE TABLE IF NOT EXISTS metric_samples(run_id TEXT NOT NULL, case_key TEXT NOT NULL, metric_id "
            "TEXT NOT NULL REFERENCES metric_descriptors(metric_id), repetition INTEGER NOT NULL CHECK(repetition >= "
            "0), value REAL NOT NULL, PRIMARY KEY(run_id, case_key, metric_id, repetition), FOREIGN KEY(run_id, "
            "case_key) REFERENCES run_cases(run_id, case_key)); CREATE TABLE IF NOT EXISTS metric_aggregates(run_id "
            "TEXT NOT NULL, case_key TEXT NOT NULL, metric_id TEXT NOT NULL REFERENCES metric_descriptors(metric_id), "
            "aggregate_name TEXT NOT NULL, value REAL NOT NULL, PRIMARY KEY(run_id, case_key, metric_id, "
            "aggregate_name), FOREIGN KEY(run_id, case_key) REFERENCES run_cases(run_id, case_key)); CREATE TABLE IF "
            "NOT EXISTS artifacts(run_id TEXT NOT NULL REFERENCES runs(run_id), relative_path TEXT NOT NULL, "
            "content_hash TEXT NOT NULL, byte_size INTEGER NOT NULL CHECK(byte_size >= 0), type TEXT NOT NULL, PRIMARY "
            "KEY(run_id, relative_path)); CREATE TABLE IF NOT EXISTS job_metadata(job_id TEXT PRIMARY KEY, "
            "metadata_json TEXT NOT NULL); CREATE INDEX IF NOT EXISTS runs_started_at_idx ON runs(started_at_utc DESC, "
            "run_id DESC); CREATE INDEX IF NOT EXISTS run_cases_lookup_idx ON run_cases(run_id, case_key); CREATE "
            "INDEX IF NOT EXISTS metric_samples_lookup_idx ON metric_samples(run_id, case_key, metric_id);";
    if (version < 1) {
        auto fail = [&](const Error& migration_error) {
            sqlite3_exec(impl->database, "ROLLBACK", nullptr, nullptr, nullptr);
            sqlite3_close(impl->database);
            delete impl;
            return std::unexpected(migration_error);
        };
        if (auto result = Sql(impl->database,
                              sqlite3_exec(impl->database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr),
                              "begin migration");
            !result)
            return fail(result.error());
        if (auto result = Sql(impl->database,
                              sqlite3_exec(impl->database, migration_v1, nullptr, nullptr, nullptr),
                              "apply migration v1");
            !result)
            return fail(result.error());
        if (auto result = ValidateV1Schema(impl->database, migration_v1); !result)
            return fail(result.error());
        if (auto result = Sql(impl->database,
                              sqlite3_exec(impl->database,
                                           "INSERT INTO schema_migrations(version, applied_at_utc) VALUES(1, "
                                           "strftime('%Y-%m-%dT%H:%M:%SZ','now'))",
                                           nullptr,
                                           nullptr,
                                           nullptr),
                              "record migration v1");
            !result)
            return fail(result.error());
        if (auto result = Sql(impl->database,
                              sqlite3_exec(impl->database, "COMMIT", nullptr, nullptr, nullptr),
                              "commit migration v1");
            !result)
            return fail(result.error());
    }
    if (auto result = ValidateV1Schema(impl->database, migration_v1); !result) {
        sqlite3_close(impl->database);
        delete impl;
        return std::unexpected(result.error());
    }
    if (auto result = RecoverPublishedDirectories(impl->database, impl->root / "runs"); !result) {
        sqlite3_close(impl->database);
        delete impl;
        return std::unexpected(result.error());
    }
    return Storage(impl);
}

Result<Run> Storage::CreateStage(const RunDraft& draft) {
    if (draft.source.empty() || draft.run_config_json.empty())
        return std::unexpected(MakeError(ErrorCode::invalid_argument, "source and run config are required"));
    const Run run{.id = NewId(),
                  .status = RunStatus::staged,
                  .source = draft.source,
                  .started_at_utc = NowUtc(),
                  .ended_at_utc = std::nullopt,
                  .run_config_json = draft.run_config_json};
    Statement statement(impl_->database,
                        "INSERT INTO "
                        "runs(run_id,status,source,started_at_utc,job_id,git_revision,environment_fingerprint,build_"
                        "fingerprint,run_config_json) VALUES(?,?,?,?,?,?,?,?,?)");
    if (auto result = statement.ready(); !result)
        return std::unexpected(result.error());
    const std::array<std::string_view, 9> values{run.id,
                                                 "staged",
                                                 draft.source,
                                                 run.started_at_utc,
                                                 draft.job_id,
                                                 draft.git_revision,
                                                 draft.environment_fingerprint,
                                                 draft.build_fingerprint,
                                                 draft.run_config_json};
    for (std::size_t index = 0; index < values.size(); ++index)
        if (auto result = Bind(impl_->database, statement.get(), static_cast<int>(index + 1U), values[index]); !result)
            return std::unexpected(result.error());
    if (auto result = Sql(impl_->database, sqlite3_step(statement.get()), "insert staged run"); !result)
        return std::unexpected(result.error());
    return run;
}

Result<void> Storage::Complete(const std::string& run_id, const std::vector<ArtifactInput>& inputs) {
    auto current = Read(run_id);
    if (!current)
        return std::unexpected(current.error());
    if (current->status != RunStatus::staged)
        return std::unexpected(MakeError(ErrorCode::conflict, "only staged runs can be completed"));
    const auto staging = impl_->root / "staging" / NewId();
    std::error_code filesystem_error;
    std::filesystem::create_directories(staging, filesystem_error);
    if (filesystem_error)
        return std::unexpected(MakeError(ErrorCode::io, "cannot create staging directory"));
    std::vector<Artifact> artifacts;
    artifacts.reserve(inputs.size());
    std::unordered_set<std::string> artifact_paths;
    for (const auto& input: inputs) {
        Artifact artifact{};
        if (auto result = CopyArtifact(input, staging, artifact); !result) {
            std::filesystem::remove_all(staging, filesystem_error);
            return std::unexpected(result.error());
        }
        if (!artifact_paths.insert(artifact.relative_path.generic_string()).second) {
            std::filesystem::remove_all(staging, filesystem_error);
            return std::unexpected(MakeError(ErrorCode::invalid_argument, "duplicate canonical artifact path"));
        }
        artifacts.push_back(std::move(artifact));
    }
    auto rollback = [&] {
        std::filesystem::remove_all(impl_->root / "runs" / run_id, filesystem_error);
        sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
    };
    if (auto result = Sql(impl_->database,
                          sqlite3_exec(impl_->database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr),
                          "begin completion");
        !result) {
        std::filesystem::remove_all(staging, filesystem_error);
        return std::unexpected(result.error());
    }
    const auto published = impl_->root / "runs" / run_id;
    current = Read(run_id);
    if (!current || current->status != RunStatus::staged) {
        std::filesystem::remove_all(staging, filesystem_error);
        sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected(current ? MakeError(ErrorCode::conflict, "only staged runs can be completed")
                                       : current.error());
    }
    if (std::filesystem::exists(published, filesystem_error) || filesystem_error) {
        std::filesystem::remove_all(staging, filesystem_error);
        sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected(MakeError(ErrorCode::conflict, "published directory already exists; recovery required"));
    }
    std::filesystem::rename(staging, published, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove_all(staging, filesystem_error);
        sqlite3_exec(impl_->database, "ROLLBACK", nullptr, nullptr, nullptr);
        return std::unexpected(MakeError(ErrorCode::io, "cannot atomically publish artifacts"));
    }
    Statement insert(impl_->database,
                     "INSERT INTO artifacts(run_id,relative_path,content_hash,byte_size,type) VALUES(?,?,?,?,?)");
    if (auto result = insert.ready(); !result) {
        rollback();
        return std::unexpected(result.error());
    }
    for (const auto& artifact: artifacts) {
        sqlite3_reset(insert.get());
        sqlite3_clear_bindings(insert.get());
        const std::array<std::string, 5> values{run_id,
                                                artifact.relative_path.generic_string(),
                                                artifact.content_hash,
                                                std::to_string(artifact.size),
                                                artifact.type};
        for (std::size_t index = 0; index < values.size(); ++index)
            if (auto result = Bind(impl_->database, insert.get(), static_cast<int>(index + 1U), values[index]);
                !result) {
                rollback();
                return std::unexpected(result.error());
            }
        if (auto result = Sql(impl_->database, sqlite3_step(insert.get()), "insert artifact"); !result) {
            rollback();
            return std::unexpected(result.error());
        }
    }
    Statement update(impl_->database,
                     "UPDATE runs SET status='completed', ended_at_utc=? WHERE run_id=? AND status='staged'");
    if (auto result = update.ready(); !result) {
        rollback();
        return std::unexpected(result.error());
    }
    const auto ended = NowUtc();
    if (auto result = Bind(impl_->database, update.get(), 1, ended); !result) {
        rollback();
        return std::unexpected(result.error());
    }
    if (auto result = Bind(impl_->database, update.get(), 2, run_id); !result) {
        rollback();
        return std::unexpected(result.error());
    }
    if (auto result = Sql(impl_->database, sqlite3_step(update.get()), "complete run");
        !result || sqlite3_changes(impl_->database) != 1) {
        rollback();
        return std::unexpected(MakeError(ErrorCode::conflict, "run completion was not applied"));
    }
    if (auto result = Sql(impl_->database,
                          sqlite3_exec(impl_->database, "COMMIT", nullptr, nullptr, nullptr),
                          "commit completion");
        !result) {
        rollback();
        return std::unexpected(result.error());
    }
    return {};
}

Result<void> Storage::Fail(const std::string& run_id, std::string_view reason) {
    if (reason.empty())
        return std::unexpected(MakeError(ErrorCode::invalid_argument, "failure reason is required"));
    Statement statement(
            impl_->database,
            "UPDATE runs SET status='failed', ended_at_utc=?, failure_reason=? WHERE run_id=? AND status='staged'");
    if (auto result = statement.ready(); !result)
        return std::unexpected(result.error());
    const auto now = NowUtc();
    if (auto result = Bind(impl_->database, statement.get(), 1, now); !result)
        return std::unexpected(result.error());
    if (auto result = Bind(impl_->database, statement.get(), 2, reason); !result)
        return std::unexpected(result.error());
    if (auto result = Bind(impl_->database, statement.get(), 3, run_id); !result)
        return std::unexpected(result.error());
    if (auto result = Sql(impl_->database, sqlite3_step(statement.get()), "fail run"); !result)
        return std::unexpected(result.error());
    if (sqlite3_changes(impl_->database) != 1)
        return std::unexpected(MakeError(ErrorCode::conflict, "only staged runs can fail"));
    return {};
}

Result<Run> Storage::Read(std::string_view run_id) const {
    Statement statement(
            impl_->database,
            "SELECT run_id,status,source,started_at_utc,ended_at_utc,run_config_json FROM runs WHERE run_id=?");
    if (auto result = statement.ready(); !result)
        return std::unexpected(result.error());
    if (auto result = Bind(impl_->database, statement.get(), 1, run_id); !result)
        return std::unexpected(result.error());
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE)
        return std::unexpected(MakeError(ErrorCode::not_found, "run does not exist"));
    if (auto result = Sql(impl_->database, step, "read run"); !result)
        return std::unexpected(result.error());
    return ReadRow(statement.get());
}

Result<RunPage> Storage::List(const std::size_t limit, const std::optional<std::string_view> after_id) const {
    if (limit == 0U || limit > 1000U)
        return std::unexpected(MakeError(ErrorCode::invalid_argument, "page limit must be 1..1000"));
    Statement statement(impl_->database,
                        "SELECT run_id,status,source,started_at_utc,ended_at_utc,run_config_json FROM runs WHERE "
                        "(?1='' OR run_id>?1) ORDER BY run_id LIMIT ?2");
    if (auto result = statement.ready(); !result)
        return std::unexpected(result.error());
    const std::string_view cursor = after_id.value_or("");
    if (auto result = Bind(impl_->database, statement.get(), 1, cursor); !result)
        return std::unexpected(result.error());
    if (auto result = Sql(impl_->database,
                          sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(limit + 1U)),
                          "bind limit");
        !result)
        return std::unexpected(result.error());
    RunPage page{};
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        if (page.runs.size() == limit) {
            page.next_cursor = page.runs.back().id;
            break;
        }
        page.runs.push_back(ReadRow(statement.get()));
    }
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        return std::unexpected(
                MakeError(ErrorCode::database, "list runs: " + std::string(sqlite3_errmsg(impl_->database))));
    return page;
}

} // namespace graphics_engine::performance_hub
