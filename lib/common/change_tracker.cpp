#include "change_tracker.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace NCommon {
namespace {

std::atomic<std::uint64_t> NEXT_TRACKER_ID{1};

void Normalize(DomainDirty& dirty) {
    if (dirty.WholeStorage) {
        dirty.Ranges.clear();
        dirty.Entities.clear();
        return;
    }
    for (ChangeEntity entity: dirty.Entities) {
        if (entity.Index == std::numeric_limits<std::uint64_t>::max()) {
            throw std::invalid_argument("Dirty entity index overflows");
        }
        dirty.Ranges.push_back({entity.Index, 1});
    }
    std::ranges::sort(dirty.Ranges, {}, &DirtyRange::Begin);
    std::size_t count = 0;
    for (DirtyRange range: dirty.Ranges) {
        if (range.Count == 0) {
            continue;
        }
        if (range.Count > std::numeric_limits<std::uint64_t>::max() - range.Begin) {
            throw std::invalid_argument("Dirty range overflows");
        }
        if (count != 0 && range.Begin <= dirty.Ranges[count - 1].Begin + dirty.Ranges[count - 1].Count) {
            DirtyRange& previous = dirty.Ranges[count - 1];
            previous.Count = std::max(previous.Begin + previous.Count, range.Begin + range.Count) - previous.Begin;
        } else {
            dirty.Ranges[count++] = range;
        }
    }
    dirty.Ranges.resize(count);
    std::ranges::sort(dirty.Entities);
    dirty.Entities.erase(std::unique(dirty.Entities.begin(), dirty.Entities.end()), dirty.Entities.end());
}

struct DomainState {
    StorageVersion Version;
    ChunkVersion DefaultChunkVersion;
    bool EntityRevisions = false;
    std::map<std::uint64_t, ChunkVersion> Chunks;
    std::map<ChangeEntity, VersionStamp> Entities;
};

struct EpochRecord {
    WorldEpoch Epoch;
    std::map<ChangeDomainKey, DomainDirty> Changes;
    std::size_t Records = 0;
};

} // namespace

struct ChangeSnapshot::State {
    ChangeCursor Cursor;
    std::uint64_t Invalidation = 0;
    WorldEpoch HistoryFloor;
    std::map<ChangeDomainKey, std::shared_ptr<const DomainState>> Domains;
    std::deque<std::shared_ptr<const EpochRecord>> History;
    std::size_t HistoryRecords = 0;
};

struct ChangeTracker::Impl {
    ChangeTrackerConfig Config;
    std::mutex Publisher;
    std::map<ChangeDomainKey, bool> Registry;
    mutable std::mutex PublishedMutex;
    std::shared_ptr<const ChangeSnapshot::State> Published;
    std::atomic<std::uint64_t> Invalidation{0};

    std::shared_ptr<const ChangeSnapshot::State> Load() const {
        std::lock_guard lock(PublishedMutex);
        return Published;
    }

    void Store(std::shared_ptr<const ChangeSnapshot::State> state) {
        std::lock_guard lock(PublishedMutex);
        Published = std::move(state);
    }
};

DomainDirty& DirtySummary::Touch(ChangeDomainKey domain) {
    for (DomainDirty& dirty: Domains) {
        if (dirty.Domain == domain) {
            return dirty;
        }
    }
    return Domains.emplace_back(DomainDirty{.Domain = domain});
}

void DirtySummary::MarkAll(ChangeDomainKey domain) {
    Touch(domain).WholeStorage = true;
}

void DirtySummary::MarkRange(ChangeDomainKey domain, std::uint64_t begin, std::uint64_t count) {
    if (count > std::numeric_limits<std::uint64_t>::max() - begin) {
        throw std::invalid_argument("Dirty range overflows");
    }
    if (count != 0) {
        Touch(domain).Ranges.push_back({begin, count});
    }
}

void DirtySummary::MarkEntity(ChangeDomainKey domain, ChangeEntity entity) {
    MarkRange(domain, entity.Index, 1);
    Touch(domain).Entities.push_back(entity);
}

bool DirtySummary::HasChanges() const noexcept {
    return AllDomains || std::ranges::any_of(Domains, [](const DomainDirty& dirty) {
               return dirty.WholeStorage || !dirty.Ranges.empty() || !dirty.Entities.empty();
           });
}

void DirtySummary::Clear() noexcept {
    AllDomains = false;
    Domains.clear();
}

ChangeTracker::ChangeTracker(const ChangeTrackerConfig& config)
    : m_impl(std::make_unique<Impl>()) {
    if (!config.InitialEpoch.IsValid() || config.ChunkSize == 0 || config.MaxDomains == 0) {
        throw std::invalid_argument("Invalid change tracker configuration");
    }
    m_impl->Config = config;
    auto state = std::make_shared<ChangeSnapshot::State>();
    // Never recycle identities, including on counter exhaustion.
    std::uint64_t id = NEXT_TRACKER_ID.load();
    do {
        if (id == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Change tracker identity exhausted");
        }
    } while (!NEXT_TRACKER_ID.compare_exchange_weak(id, id + 1));
    state->Cursor = {id, config.InitialEpoch};
    state->HistoryFloor = config.InitialEpoch;
    m_impl->Store(std::move(state));
}

ChangeTracker::~ChangeTracker() = default;

void ChangeTracker::RegisterDomain(ChangeDomainKey domain, bool entityRevisions) {
    if (domain.OwnerId == 0 || domain.TypeId == 0 || domain.Generation == 0) {
        throw std::invalid_argument("Invalid change domain identity");
    }
    std::lock_guard lock(m_impl->Publisher);
    for (auto iterator = m_impl->Registry.begin(); iterator != m_impl->Registry.end(); ++iterator) {
        if (iterator->first.OwnerId != domain.OwnerId || iterator->first.TypeId != domain.TypeId) {
            continue;
        }
        if (domain.Generation < iterator->first.Generation) {
            throw std::invalid_argument("Stale change domain generation");
        }
        if (domain.Generation == iterator->first.Generation) {
            iterator->second = iterator->second || entityRevisions;
            return;
        }
        // Allocate the replacement before removing the old identity.
        m_impl->Registry.emplace(domain, entityRevisions);
        m_impl->Registry.erase(iterator);
        return;
    }
    if (m_impl->Registry.size() >= m_impl->Config.MaxDomains) {
        throw std::length_error("Change domain limit exceeded");
    }
    m_impl->Registry.emplace(domain, entityRevisions);
}

// Build a replacement root under publisher serialization, then swap it at one boundary.
// Registration changes and clock wrap force full invalidation; otherwise local summaries
// merge into bounded sparse versions and retained epoch records. Any allocation/validation
// failure leaves the old root pinned and invalidates live captures until recovery.
WorldEpoch ChangeTracker::PublishEpoch(std::span<const DirtySummary* const> summaries) {
    std::lock_guard lock(m_impl->Publisher);
    try {
        const auto old = m_impl->Load();
        auto state = std::make_shared<ChangeSnapshot::State>(*old);
        // Captures compare this sequence with live invalidation, so a later concurrent
        // invalidation remains visible even after this replacement root is installed.
        state->Invalidation = m_impl->Invalidation.load(std::memory_order_acquire);
        bool full = state->Invalidation != old->Invalidation;
        WorldEpoch& epoch = state->Cursor.Epoch;
        if (epoch.Value == std::numeric_limits<std::uint64_t>::max()) {
            if (epoch.Era == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("Change epoch exhausted");
            }
            epoch = {epoch.Era + 1, 1};
            full = true;
            state->History.clear();
            state->HistoryRecords = 0;
            state->HistoryFloor = epoch;
        } else {
            ++epoch.Value;
        }
        auto record = std::make_shared<EpochRecord>();
        record->Epoch = epoch;
        for (const DirtySummary* summary: summaries) {
            if (summary == nullptr) {
                throw std::invalid_argument("Null dirty summary");
            }
            full = full || summary->AllDomains;
            for (const DomainDirty& source: summary->Domains) {
                if (!m_impl->Registry.contains(source.Domain)) {
                    throw std::invalid_argument("Unregistered or stale change domain");
                }
                DomainDirty& target = record->Changes[source.Domain];
                target.Domain = source.Domain;
                target.WholeStorage = target.WholeStorage || source.WholeStorage;
                target.Ranges.insert(target.Ranges.end(), source.Ranges.begin(), source.Ranges.end());
                target.Entities.insert(target.Entities.end(), source.Entities.begin(), source.Entities.end());
            }
        }
        std::erase_if(state->Domains, [&](const auto& item) { return !m_impl->Registry.contains(item.first); });
        for (const auto& [key, revisions]: m_impl->Registry) {
            const auto previous = state->Domains.find(key);
            const bool initial = previous == state->Domains.end();
            const bool revisionChange = !initial && previous->second->EntityRevisions != revisions;
            if (full || initial || revisionChange) {
                record->Changes[key].Domain = key;
                record->Changes[key].WholeStorage = true;
            }
            auto change = record->Changes.find(key);
            if (change == record->Changes.end()) {
                continue;
            }
            DomainDirty& dirty = change->second;
            Normalize(dirty);
            if (!dirty.WholeStorage && dirty.Ranges.empty() && dirty.Entities.empty()) {
                record->Changes.erase(change);
                continue;
            }
            auto domain = initial ? std::make_shared<DomainState>() : std::make_shared<DomainState>(*previous->second);
            domain->EntityRevisions = revisions;
            domain->Version = epoch;
            if (dirty.Ranges.size() > m_impl->Config.MaxRangesPerDomain) {
                dirty.WholeStorage = true;
            }
            if (!dirty.WholeStorage) {
                for (DirtyRange range: dirty.Ranges) {
                    const std::uint64_t last = (range.Begin + range.Count - 1) / m_impl->Config.ChunkSize;
                    std::uint64_t chunk = range.Begin / m_impl->Config.ChunkSize;
                    if (last - chunk >= m_impl->Config.MaxChunkVersionsPerDomain) {
                        dirty.WholeStorage = true;
                        break;
                    }
                    for (; chunk <= last; ++chunk) {
                        domain->Chunks[chunk] = epoch;
                    }
                    if (domain->Chunks.size() > m_impl->Config.MaxChunkVersionsPerDomain) {
                        dirty.WholeStorage = true;
                        break;
                    }
                }
                if (revisions) {
                    // A range-only write cannot preserve an earlier precise entity revision.
                    std::erase_if(domain->Entities, [&](const auto& item) {
                        return std::ranges::any_of(dirty.Ranges, [&](DirtyRange range) {
                            return item.first.Index >= range.Begin && item.first.Index - range.Begin < range.Count;
                        });
                    });
                    for (ChangeEntity entity: dirty.Entities) {
                        domain->Entities[entity] = epoch;
                    }
                    dirty.WholeStorage =
                            dirty.WholeStorage || domain->Entities.size() > m_impl->Config.MaxEntityVersionsPerDomain;
                }
            }
            if (dirty.WholeStorage) {
                // Coarse invalidation retires every earlier precise stamp: retaining
                // sparse entity revisions here could incorrectly certify stale data.
                domain->DefaultChunkVersion = epoch;
                domain->Chunks.clear();
                domain->Entities.clear();
                Normalize(dirty);
            }
            record->Records += 1 + dirty.Ranges.size() + dirty.Entities.size();
            state->Domains[key] = std::move(domain);
        }
        state->History.push_back(record);
        state->HistoryRecords += record->Records;
        while (state->History.size() > m_impl->Config.MaxHistoryEpochs ||
               state->HistoryRecords > m_impl->Config.MaxHistoryRecords) {
            // A cursor at the evicted epoch has already consumed that record. Only
            // cursors strictly below this floor need a conservative full replay.
            state->HistoryFloor = state->History.front()->Epoch;
            state->HistoryRecords -= state->History.front()->Records;
            state->History.pop_front();
        }
        const WorldEpoch publishedEpoch = epoch;
        m_impl->Store(std::move(state));
        return publishedEpoch;
    } catch (...) {
        InvalidateAll();
        throw;
    }
}

void ChangeTracker::InvalidateAll() noexcept {
    std::uint64_t value = m_impl->Invalidation.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max() &&
           !m_impl->Invalidation.compare_exchange_weak(value,
                                                       value + 1,
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed)) {
    }
}

ChangeSnapshot ChangeTracker::CaptureSnapshot(bool forceFull) const {
    ChangeSnapshot snapshot;
    snapshot.m_state = m_impl->Load();
    const std::uint64_t invalidation = m_impl->Invalidation.load(std::memory_order_acquire);
    snapshot.m_forceFull = forceFull || invalidation != snapshot.m_state->Invalidation ||
                           invalidation == std::numeric_limits<std::uint64_t>::max();
    return snapshot;
}

ChangeCapture ChangeTracker::CaptureChanges(ChangeCursor since, ChangeDomainKey domain) const {
    return CaptureSnapshot().CaptureChanges(since, domain);
}

ChangeCursor ChangeSnapshot::GetCursor() const {
    if (!m_state) {
        throw std::logic_error("Empty change snapshot");
    }
    return m_state->Cursor;
}

ChangeCapture ChangeSnapshot::CaptureChanges(ChangeCursor since, ChangeDomainKey domain) const {
    const ChangeCursor cursor = GetCursor();
    const StorageVersion version = GetStorageVersion(domain);
    if (since.TrackerId != 0 &&
        (since.TrackerId != cursor.TrackerId || since.Epoch > cursor.Epoch || !since.Epoch.IsValid())) {
        throw std::invalid_argument("Foreign or future change cursor");
    }
    ChangeCapture result{.Cursor = cursor, .Version = version};
    if (m_forceFull || since.TrackerId == 0) {
        result.FullDirty = true;
        return result;
    }
    if (version <= since.Epoch) {
        return result;
    }
    if (since.Epoch < m_state->HistoryFloor || since.Epoch.Era != cursor.Epoch.Era) {
        result.FullDirty = true;
        return result;
    }
    DomainDirty merged{.Domain = domain};
    for (const auto& record: m_state->History) {
        if (record->Epoch <= since.Epoch) {
            continue;
        }
        const auto found = record->Changes.find(domain);
        if (found == record->Changes.end()) {
            continue;
        }
        merged.WholeStorage = merged.WholeStorage || found->second.WholeStorage;
        merged.Ranges.insert(merged.Ranges.end(), found->second.Ranges.begin(), found->second.Ranges.end());
        merged.Entities.insert(merged.Entities.end(), found->second.Entities.begin(), found->second.Entities.end());
    }
    Normalize(merged);
    result.FullDirty = merged.WholeStorage;
    result.Ranges = std::move(merged.Ranges);
    result.Entities = std::move(merged.Entities);
    return result;
}

StorageVersion ChangeSnapshot::GetStorageVersion(ChangeDomainKey domain) const {
    static_cast<void>(GetCursor());
    const auto found = m_state->Domains.find(domain);
    if (found == m_state->Domains.end()) {
        throw std::invalid_argument("Unpublished or stale change domain");
    }
    return m_forceFull ? StorageVersion{} : found->second->Version;
}

ChunkVersion ChangeSnapshot::GetChunkVersion(ChangeDomainKey domain, std::uint64_t chunk) const {
    static_cast<void>(GetStorageVersion(domain));
    if (m_forceFull) {
        return {};
    }
    const DomainState& state = *m_state->Domains.at(domain);
    const auto found = state.Chunks.find(chunk);
    return found == state.Chunks.end() ? state.DefaultChunkVersion : found->second;
}

std::optional<VersionStamp> ChangeSnapshot::GetEntityRevision(ChangeDomainKey domain, ChangeEntity entity) const {
    static_cast<void>(GetStorageVersion(domain));
    const DomainState& state = *m_state->Domains.at(domain);
    const auto found = state.Entities.find(entity);
    if (m_forceFull || !state.EntityRevisions || found == state.Entities.end()) {
        return std::nullopt;
    }
    return found->second;
}

ChangeTrackerStatistics ChangeSnapshot::GetStatistics() const {
    static_cast<void>(GetCursor());
    ChangeTrackerStatistics result{.Domains = m_state->Domains.size(),
                                   .HistoryEpochs = m_state->History.size(),
                                   .HistoryRecords = m_state->HistoryRecords};
    for (const auto& [key, domain]: m_state->Domains) {
        static_cast<void>(key);
        result.ChunkVersions += domain->Chunks.size();
        result.EntityVersions += domain->Entities.size();
    }
    return result;
}

} // namespace NCommon
