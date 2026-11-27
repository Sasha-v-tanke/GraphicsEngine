#include "tools/performance_hub/storage.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sqlite3.h>

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
        CHECK(std::filesystem::exists(root / "runs" / first->id / "reports/report.json"));
        CHECK(!storage->Complete(first->id, artifacts).has_value());
        CHECK(storage->Fail(second->id, "process crashed").has_value());
        auto failed = storage->Read(second->id);
        CHECK(failed.has_value());
        CHECK(failed->status == ph::RunStatus::failed);
    }
    auto reopened = ph::Storage::Open(root);
    CHECK(reopened.has_value());
    auto persisted = reopened->Read(first_id);
    CHECK(persisted.has_value());
    CHECK(persisted->status == ph::RunStatus::completed);
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
} // namespace

int main() {
    const auto root = TemporaryRoot();
    PersistenceAndTerminalStates(root / "persistence");
    RejectsUnsafeArtifacts(root / "unsafe");
    OldSchemaMigrationAndForeignKeys(root / "migration");
    RecoversPublishedDirectoryAfterInterruptedCompletion(root / "recovery");
    RejectsIncompatibleLegacySchema(root / "incompatible");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return failures == 0 ? 0 : 1;
}
