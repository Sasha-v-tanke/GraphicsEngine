#include "tools/performance_hub/storage.h"

#include <filesystem>
#include <fstream>
#include <iostream>

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
    auto unchanged = storage->Read(run->id);
    CHECK(unchanged.has_value());
    CHECK(unchanged->status == ph::RunStatus::staged);
}

void OldSchemaMigrationAndForeignKeys(const std::filesystem::path& root) {
    const auto database = root / "performance-hub.sqlite3";
    {
        std::ofstream file(database);
    }
    auto storage = ph::Storage::Open(root);
    CHECK(storage.has_value());
    auto page = storage->List(10);
    CHECK(page.has_value());
    CHECK(page->runs.empty());
    CHECK(!storage->List(0).has_value());
}
} // namespace

int main() {
    const auto root = TemporaryRoot();
    PersistenceAndTerminalStates(root / "persistence");
    RejectsUnsafeArtifacts(root / "unsafe");
    OldSchemaMigrationAndForeignKeys(root / "migration");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return failures == 0 ? 0 : 1;
}
