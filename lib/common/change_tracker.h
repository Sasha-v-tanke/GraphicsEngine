#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NCommon {

struct VersionStamp {
    std::uint64_t Era = 0;
    std::uint64_t Value = 0;
    [[nodiscard]] bool IsValid() const noexcept {
        return Era != 0;
    }
    friend auto operator<=>(VersionStamp, VersionStamp) = default;
};

using WorldEpoch = VersionStamp;
using StorageVersion = VersionStamp;
using ChunkVersion = VersionStamp;

struct ChangeDomainKey {
    std::uint64_t OwnerId = 0;
    std::uint64_t TypeId = 0;
    std::uint64_t Generation = 0;
    friend auto operator<=>(ChangeDomainKey, ChangeDomainKey) = default;
};

struct DirtyRange {
    std::uint64_t Begin = 0;
    std::uint64_t Count = 0;
    friend bool operator==(DirtyRange, DirtyRange) = default;
};

struct ChangeEntity {
    std::uint64_t Index = 0;
    std::uint64_t Generation = 0;
    friend auto operator<=>(ChangeEntity, ChangeEntity) = default;
};

struct DomainDirty {
    ChangeDomainKey Domain = {};
    bool WholeStorage = false;
    std::vector<DirtyRange> Ranges = {};
    std::vector<ChangeEntity> Entities = {};
};

// Caller/batch owned. No shared lock, atomic counter or implicit publication on writes.
struct DirtySummary {
    bool AllDomains = false;
    std::vector<DomainDirty> Domains;
    DomainDirty& Touch(ChangeDomainKey domain);
    void MarkAll(ChangeDomainKey domain);
    void MarkRange(ChangeDomainKey domain, std::uint64_t begin, std::uint64_t count);
    void MarkEntity(ChangeDomainKey domain, ChangeEntity entity);
    [[nodiscard]] bool HasChanges() const noexcept;
    void Clear() noexcept;
};

struct ChangeCursor {
    std::uint64_t TrackerId = 0;
    WorldEpoch Epoch;
    friend bool operator==(ChangeCursor, ChangeCursor) = default;
};

struct ChangeCapture {
    ChangeCursor Cursor = {};
    StorageVersion Version = {};
    bool FullDirty = false;
    std::vector<DirtyRange> Ranges = {};
    std::vector<ChangeEntity> Entities = {};
    [[nodiscard]] bool HasChanges() const noexcept {
        return FullDirty || !Ranges.empty();
    }
};

struct ChangeTrackerConfig {
    std::size_t MaxDomains = 1024;
    std::size_t MaxHistoryEpochs = 32;
    std::size_t MaxHistoryRecords = 16384;
    std::size_t MaxRangesPerDomain = 1024;
    std::size_t MaxChunkVersionsPerDomain = 1024;
    std::size_t MaxEntityVersionsPerDomain = 1024;
    std::uint64_t ChunkSize = 256;
    // Initial clock supports restoring/testing a publication boundary; tracker identity remains new.
    WorldEpoch InitialEpoch{1, 0};
};

struct ChangeTrackerStatistics {
    std::size_t Domains = 0;
    std::size_t HistoryEpochs = 0;
    std::size_t HistoryRecords = 0;
    std::size_t ChunkVersions = 0;
    std::size_t EntityVersions = 0;
};

// Immutable metadata only. This does not authorize concurrent reads of mutable owner data.
class ChangeSnapshot final {
public:
    ChangeSnapshot() = default;
    [[nodiscard]] ChangeCursor GetCursor() const;
    [[nodiscard]] ChangeCapture CaptureChanges(ChangeCursor since, ChangeDomainKey domain) const;
    [[nodiscard]] StorageVersion GetStorageVersion(ChangeDomainKey domain) const;
    [[nodiscard]] ChunkVersion GetChunkVersion(ChangeDomainKey domain, std::uint64_t chunk) const;
    [[nodiscard]] std::optional<VersionStamp> GetEntityRevision(ChangeDomainKey domain, ChangeEntity entity) const;
    [[nodiscard]] ChangeTrackerStatistics GetStatistics() const;

private:
    struct State;
    std::shared_ptr<const State> m_state;
    bool m_forceFull = false;
    friend class ChangeTracker;
};

class ChangeTracker final: private NonTransferable {
public:
    explicit ChangeTracker(ChangeTrackerConfig config = {});
    ~ChangeTracker();
    // Publisher-side registration. Same owner/type may only advance generation; visible at next PublishEpoch.
    void RegisterDomain(ChangeDomainKey domain, bool entityRevisions = false);
    // One call = one coherent epoch, after all batch writers retire. Local summaries are not consumed.
    WorldEpoch PublishEpoch(std::span<const DirtySummary* const> summaries = std::span<const DirtySummary* const>());
    // Allocation-free conservative fallback. A concurrent invalidation cannot be erased by a publisher.
    void InvalidateAll() noexcept;
    [[nodiscard]] ChangeSnapshot CaptureSnapshot(bool forceFull = false) const;
    [[nodiscard]] ChangeCapture CaptureChanges(ChangeCursor since, ChangeDomainKey domain) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace NCommon
