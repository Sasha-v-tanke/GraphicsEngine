#include "tools/performance_hub/storage.h"
#include "tools/performance_hub/storage_internal.h"

#include <barrier>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <sstream>
#include <thread>

namespace ph = graphics_engine::performance_hub;

namespace {
int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n";                                          \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

std::filesystem::path TemporaryRoot() {
    const auto root = std::filesystem::temp_directory_path() / "graphics-engine-performance-hub-tests";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    return root;
}

ph::RunDraft Draft() {
    return {.source = "local",
            .job_id = "job",
            .git_revision = "abc",
            .environment_fingerprint = "mac",
            .build_fingerprint = "debug",
            .run_config_json = "{\"repetitions\":3}"};
}

void CheckMetadata(const ph::Run& run) {
    const auto draft = Draft();
    CHECK(run.source == draft.source);
    CHECK(run.job_id == draft.job_id);
    CHECK(run.git_revision == draft.git_revision);
    CHECK(run.environment_fingerprint == draft.environment_fingerprint);
    CHECK(run.build_fingerprint == draft.build_fingerprint);
    CHECK(run.run_config_json == draft.run_config_json);
}

void PersistenceAndTerminalStates(const std::filesystem::path& root) {
    std::string first_id;
    {
        auto storage = ph::Storage::Open(root);
        CHECK(storage.has_value());
        auto first = storage->CreateStage(Draft());
        auto second = storage->CreateStage(Draft());
        CHECK(first.has_value());
        CHECK(second.has_value());
        CHECK(first->id != second->id);
        first_id = first->id;
        CheckMetadata(*first);
        const auto source = root / "report.json";
        {
            std::ofstream file(source);
            file << "{}";
        }
        const std::vector<ph::ArtifactInput> artifacts{
                {.source_file = source, .relative_path = "reports/report.json", .type = "benchmark-json"}};
        CHECK(storage->Complete(first->id, artifacts).has_value());
        auto completed = storage->Read(first->id);
        CHECK(completed.has_value());
        CHECK(completed->status == ph::RunStatus::completed);
        CheckMetadata(*completed);
        CHECK(std::filesystem::exists(root / "runs" / first->id / "reports/report.json"));
        CHECK(!storage->Complete(first->id, artifacts).has_value());
        CHECK(storage->Fail(second->id, "process crashed").has_value());
        auto failed = storage->Read(second->id);
        CHECK(failed.has_value());
        CHECK(failed->status == ph::RunStatus::failed);
        CHECK(failed->failure_reason == "process crashed");
    }
    auto reopened = ph::Storage::Open(root);
    CHECK(reopened.has_value());
    auto persisted = reopened->Read(first_id);
    CHECK(persisted.has_value());
    CHECK(persisted->status == ph::RunStatus::completed);
    CheckMetadata(*persisted);
    const auto page = reopened->List(10);
    CHECK(page.has_value());
    if (page)
        for (const auto& run: page->runs)
            CheckMetadata(run);
}

void PublishedArtifactIntegrity(const std::filesystem::path& root) {
    std::string run_id;
    const auto original = root / "original.json";
    {
        auto storage = ph::Storage::Open(root);
        CHECK(storage.has_value());
        if (!storage)
            return;
        auto staged = storage->CreateStage(Draft());
        CHECK(staged.has_value());
        if (!staged)
            return;
        run_id = staged->id;
        {
            std::ofstream file(original);
            file << "good";
        }
        CHECK(storage->Complete(run_id, {{.source_file = original, .relative_path = "report.json", .type = "json"}})
                      .has_value());
    }
    auto storage = ph::Storage::Open(root);
    CHECK(storage.has_value());
    if (!storage)
        return;
    CHECK(storage->Read(run_id).has_value());
    const auto published = root / "runs" / run_id / "report.json";
    // Same size but different bytes must fail the hash check after restart.
    {
        std::ofstream file(published);
        file << "evil";
    }
    auto changed = storage->Read(run_id);
    CHECK(!changed.has_value());
    if (!changed)
        CHECK(changed.error().code == ph::ErrorCode::artifact_corrupt);
    {
        std::ofstream file(published);
        file << "g";
    }
    auto truncated = storage->Read(run_id);
    CHECK(!truncated.has_value());
    if (!truncated)
        CHECK(truncated.error().code == ph::ErrorCode::artifact_corrupt);
    {
        std::ofstream file(published);
        file << "good";
    }
    CHECK(storage->Read(run_id).has_value());
    std::filesystem::remove(published);
    auto missing = storage->Read(run_id);
    CHECK(!missing.has_value());
    if (!missing)
        CHECK(missing.error().code == ph::ErrorCode::artifact_missing);
    // Integrity failures must neither alter the immutable record nor hide it from metadata history.
    const auto page = storage->List(10);
    CHECK(page.has_value());
    if (page) {
        CHECK(page->runs.size() == 1);
        if (!page->runs.empty())
            CHECK(page->runs.front().status == ph::RunStatus::completed);
    }
    {
        std::ofstream file(published);
        file << "good";
    }
    CHECK(storage->Read(run_id).has_value());
}

void RejectsUnsafeArtifacts(const std::filesystem::path& root) {
    auto storage = ph::Storage::Open(root);
    CHECK(storage.has_value());
    auto run = storage->CreateStage(Draft());
    CHECK(run.has_value());
    const auto source = root / "input.log";
    {
        std::ofstream file(source);
        file << "log";
    }
    CHECK(!storage->Complete(run->id, {{.source_file = source, .relative_path = "../escape.log", .type = "log"}})
                   .has_value());
    CHECK(!storage->Complete(run->id, {{.source_file = source, .relative_path = "a/../b", .type = "log"}}).has_value());
    CHECK(!storage->Complete(run->id, {{.source_file = root / "missing", .relative_path = "log.txt", .type = "log"}})
                   .has_value());
    CHECK(!storage->Complete(run->id,
                             {{.source_file = source, .relative_path = "report.json", .type = "log"},
                              {.source_file = source, .relative_path = "./report.json", .type = "log"}})
                   .has_value());
    auto unchanged = storage->Read(run->id);
    CHECK(unchanged.has_value());
    CHECK(unchanged->status == ph::RunStatus::staged);
}

void OldSchemaMigrationAndForeignKeys(const std::filesystem::path& root) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    CHECK(!filesystem_error);
    const auto database = root / "performance-hub.sqlite3";
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open(database.c_str(), &raw) == SQLITE_OK);
    CHECK(sqlite3_exec(raw,
                       "CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, applied_at_utc TEXT NOT NULL)",
                       nullptr,
                       nullptr,
                       nullptr) == SQLITE_OK);
    CHECK(sqlite3_exec(raw, "INSERT INTO schema_migrations VALUES(0, 'old')", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
    auto storage = ph::Storage::Open(root);
    CHECK(storage.has_value());
    auto page = storage->List(10);
    CHECK(page.has_value());
    CHECK(page->runs.empty());
    CHECK(!storage->List(0).has_value());
    sqlite3* newer = nullptr;
    CHECK(sqlite3_open(database.c_str(), &newer) == SQLITE_OK);
    CHECK(sqlite3_exec(newer, "INSERT INTO schema_migrations VALUES(2, 'future')", nullptr, nullptr, nullptr) ==
          SQLITE_OK);
    sqlite3_close(newer);
    CHECK(!ph::Storage::Open(root).has_value());
}

void RecoversPublishedDirectoryAfterInterruptedCompletion(const std::filesystem::path& root) {
    std::string run_id;
    {
        auto storage = ph::Storage::Open(root);
        CHECK(storage.has_value());
        auto run = storage->CreateStage(Draft());
        CHECK(run.has_value());
        run_id = run->id;
        std::error_code error;
        std::filesystem::create_directories(root / "runs" / run_id, error);
        CHECK(!error);
    }
    auto recovered = ph::Storage::Open(root);
    CHECK(recovered.has_value());
    CHECK(!std::filesystem::exists(root / "runs" / run_id));
    const auto source = root / "result.log";
    {
        std::ofstream file(source);
        file << "result";
    }
    CHECK(recovered->Complete(run_id, {{.source_file = source, .relative_path = "result.log", .type = "log"}})
                  .has_value());
}

void RejectsIncompatibleLegacySchema(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open((root / "performance-hub.sqlite3").c_str(), &raw) == SQLITE_OK);
    CHECK(sqlite3_exec(raw,
                       "CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, applied_at_utc TEXT NOT NULL); "
                       "INSERT INTO schema_migrations VALUES(0, 'old'); CREATE TABLE runs(unrelated TEXT)",
                       nullptr,
                       nullptr,
                       nullptr) == SQLITE_OK);
    sqlite3_close(raw);
    CHECK(!ph::Storage::Open(root).has_value());
}

void RejectsCorruptedCurrentSchema(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open((root / "performance-hub.sqlite3").c_str(), &raw) == SQLITE_OK);
    CHECK(sqlite3_exec(raw,
                       "CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, applied_at_utc TEXT NOT NULL); "
                       "INSERT INTO schema_migrations VALUES(1, 'current')",
                       nullptr,
                       nullptr,
                       nullptr) == SQLITE_OK);
    sqlite3_close(raw);
    CHECK(!ph::Storage::Open(root).has_value());
}
void IntegrityAndErrors(const std::filesystem::path& root) {
    auto storage = ph::Storage::Open(root);
    CHECK(storage.has_value());
    if (!storage)
        return;
    auto run = storage->CreateStage(Draft());
    CHECK(run.has_value());
    if (!run)
        return;
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open((root / "performance-hub.sqlite3").c_str(), &raw) == SQLITE_OK);
    CHECK(sqlite3_exec(raw, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr) == SQLITE_OK);
    auto page = storage->List(10);
    CHECK(!page.has_value());
    if (!page)
        CHECK(page.error().code == ph::ErrorCode::database);
    CHECK(sqlite3_exec(raw, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);
    CHECK(storage->List(10).has_value());
    CHECK(sqlite3_exec(
                  raw,
                  "CREATE TRIGGER fail_completion BEFORE UPDATE ON runs BEGIN SELECT RAISE(ABORT, 'injected'); END",
                  nullptr,
                  nullptr,
                  nullptr) == SQLITE_OK);
    CHECK(!storage->Complete(run->id, {}).has_value());
    CHECK(!std::filesystem::exists(root / "runs" / run->id));
    CHECK(storage->Read(run->id)->status == ph::RunStatus::staged);
    CHECK(sqlite3_exec(raw, "DROP TRIGGER fail_completion", nullptr, nullptr, nullptr) == SQLITE_OK);
    CHECK(storage->Complete(run->id, {}).has_value());
    CHECK(sqlite3_exec(raw, "DROP INDEX runs_started_at_idx", nullptr, nullptr, nullptr) == SQLITE_OK);
    CHECK(!ph::Storage::Open(root).has_value());
    CHECK(sqlite3_exec(raw,
                       "CREATE INDEX runs_started_at_idx ON runs(started_at_utc DESC,run_id DESC); DROP TABLE "
                       "artifacts; CREATE TABLE artifacts(run_id TEXT,relative_path TEXT,content_hash TEXT,byte_size "
                       "INTEGER,type TEXT)",
                       nullptr,
                       nullptr,
                       nullptr) == SQLITE_OK);
    CHECK(!ph::Storage::Open(root).has_value());
    sqlite3_close(raw);
}

void ConcurrentCompletion(const std::filesystem::path& root) {
    auto first = ph::Storage::Open(root);
    auto second = ph::Storage::Open(root);
    CHECK(first && second);
    if (!first || !second)
        return;
    auto run = first->CreateStage(Draft());
    CHECK(run.has_value());
    if (!run)
        return;
    std::barrier start(2);
    bool first_ok = false;
    bool second_ok = false;
    std::thread writer([&] {
        start.arrive_and_wait();
        first_ok = first->Complete(run->id, {}).has_value();
    });
    start.arrive_and_wait();
    second_ok = second->Complete(run->id, {}).has_value();
    writer.join();
    CHECK(first_ok != second_ok);
    CHECK(first->Read(run->id)->status == ph::RunStatus::completed);
    CHECK(std::filesystem::is_directory(root / "runs" / run->id));
}

void StreamingBoundsAndErrors() {
    std::istringstream growing(std::string(16385, 'x'));
    std::ostringstream output;
    CHECK(!ph::detail::CopyArtifactBytes(growing, output, 16384).has_value());
    CHECK(output.str().size() <= 16384);
    std::istringstream exact(std::string(16384, 'x'));
    std::ostringstream exact_output;
    auto copied = ph::detail::CopyArtifactBytes(exact, exact_output, 16384);
    CHECK(copied.has_value());
    if (copied)
        CHECK(copied->bytes == exact_output.str().size());
    std::istringstream broken("data");
    broken.setstate(std::ios::badbit);
    CHECK(!ph::detail::CopyArtifactBytes(broken, output, 16384).has_value());
    std::istringstream valid("data");
    std::ostringstream broken_output;
    broken_output.setstate(std::ios::badbit);
    CHECK(!ph::detail::CopyArtifactBytes(valid, broken_output, 16384).has_value());
}
} // namespace

int main() {
    const auto root = TemporaryRoot();
    PersistenceAndTerminalStates(root / "persistence");
    PublishedArtifactIntegrity(root / "artifact-integrity");
    RejectsUnsafeArtifacts(root / "unsafe");
    OldSchemaMigrationAndForeignKeys(root / "migration");
    RecoversPublishedDirectoryAfterInterruptedCompletion(root / "recovery");
    RejectsIncompatibleLegacySchema(root / "incompatible");
    RejectsCorruptedCurrentSchema(root / "corrupted-current");
    IntegrityAndErrors(root / "integrity");
    ConcurrentCompletion(root / "concurrent");
    StreamingBoundsAndErrors();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return failures == 0 ? 0 : 1;
}
