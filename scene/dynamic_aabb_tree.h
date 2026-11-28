#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <GraphicsEngine/scene/static_bvh.h>

namespace NScene {

struct DynamicAabbTreeConfig {
    std::size_t MaxRecords = 1'000'000;
    double FatMargin = 0.5;
};

// Per-publication work counters; generation replacement is remove + insert.
struct DynamicAabbTreeStatistics {
    std::uint64_t Version = 0;
    std::size_t IndexedRecords = 0;
    std::size_t SkippedRecords = 0;
    std::size_t Inserted = 0;
    std::size_t Removed = 0;
    std::size_t Reinserted = 0;
    std::size_t UpdatedInFatBounds = 0;
    std::size_t Rotations = 0;
    std::size_t MaxDepth = 0;
    std::size_t AllocatedNodes = 0;
};

// Value-owned immutable CPU snapshot. Build/Refresh consume authoritative committed
// spatial records for one Scene; caller synchronizes publication at the safe boundary.
// Neither operation reads/mutates ECS or waits on workers. Copies retain their version.
class DynamicAabbTree final {
public:
    [[nodiscard]] static DynamicAabbTree Build(SceneHandle scene,
                                               std::uint64_t version,
                                               std::span<const SpatialRecord> records,
                                               DynamicAabbTreeConfig config = {});
    // A complete input snapshot, not a delta. Only VALID explicitly DYNAMIC records
    // are indexed. Absent identities are removed, new generations are inserted, and
    // movement beyond fat bounds reinserts a leaf. Requires a strictly newer version.
    // Malformed included records, duplicate slots, mixed Worlds and budget overflow
    // throw INVALID_ARGUMENT; the previous snapshot is unaffected.
    [[nodiscard]] DynamicAabbTree Refresh(std::uint64_t version, std::span<const SpatialRecord> records) const;

    [[nodiscard]] SceneHandle GetScene() const noexcept;
    [[nodiscard]] std::uint64_t GetVersion() const noexcept;
    [[nodiscard]] const DynamicAabbTreeStatistics& GetStatistics() const noexcept;

    // Same conservative coordinate, candidate and ordering contract as StaticBvh.
    // Fat bounds prune internal traversal; tight leaf bounds determine candidates.
    [[nodiscard]] SpatialQueryResult QueryOverlap(SpatialAabb bounds, SpatialVector origin = {}) const;
    [[nodiscard]] SpatialQueryResult QueryRay(SpatialRay ray, SpatialVector origin = {}) const;
    [[nodiscard]] SpatialQueryResult QueryFrustum(const SpatialFrustum& frustum, SpatialVector origin = {}) const;

private:
    struct Data;
    explicit DynamicAabbTree(std::shared_ptr<const Data> data);
    std::shared_ptr<const Data> m_data;
};

} // namespace NScene
