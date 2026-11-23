#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <GraphicsEngine/scene/spatial_bounds.h>

namespace NScene {

struct StaticBvhConfig {
    std::size_t MaxRecords = 1'000'000;
    std::uint32_t LeafCapacity = 8;
};

struct StaticBvhNode {
    SpatialAabb Bounds;
    std::uint32_t Left = 0;
    std::uint32_t Right = 0;
    std::uint32_t First = 0;
    // Nonzero for leaves; First indexes the snapshot's private item permutation.
    std::uint32_t Count = 0;
    friend bool operator==(const StaticBvhNode&, const StaticBvhNode&) = default;
};

struct StaticBvhStatistics {
    std::uint64_t Version = 0;
    std::size_t InputRecords = 0;
    std::size_t IndexedRecords = 0;
    std::size_t SkippedRecords = 0;
    std::size_t Nodes = 0;
    std::size_t Leaves = 0;
    std::size_t MaxDepth = 0;
    // Owned allocation capacities, excluding allocator/control-block overhead and caller input/results.
    std::size_t StorageBytes = 0;
    [[nodiscard]] std::string Serialize() const;
};

struct SpatialQueryStatistics {
    std::size_t VisitedNodes = 0;
    std::size_t TestedItems = 0;
    std::size_t MatchedItems = 0;
    [[nodiscard]] std::string Serialize() const;
};

struct SpatialQueryResult {
    std::uint64_t Version = 0;
    std::vector<SpatialIdentity> Candidates;
    SpatialQueryStatistics Statistics;
};

struct SpatialRay {
    SpatialVector Origin = {};
    SpatialVector Direction{1, 0, 0};
    // Parameter t in Origin + t * Direction; direction need not be normalized.
    double MinDistance = 0;
    double MaxDistance = std::numeric_limits<double>::infinity();
};

struct SpatialPlane {
    SpatialVector Normal;
    // Inside is dot(Normal, point) + Offset >= 0, including boundary contact.
    double Offset = 0;
};

struct SpatialFrustum {
    std::array<SpatialPlane, 6> Planes;
};

// Value-copyable handle to immutable versioned nodes/identities, with no Scene,
// World or component ownership. Concurrent const queries need no external lock.
// Build/refit consume value snapshots outside worker-critical Update; caller may
// schedule them through the existing TaskSystem, then publish a const shared_ptr
// at its own safe boundary. No worker waits, scheduler or global publication state.
class StaticBvh final {
public:
    // Includes only VALID explicitly STATIC records. Malformed included records,
    // foreign Scene/mixed Worlds, duplicate identities, zero version or budget overflow
    // throw INVALID_ARGUMENT. Invalid/dynamic/unclassified records are counted as skipped.
    [[nodiscard]] static StaticBvh Build(SceneHandle scene,
                                         std::uint64_t version,
                                         std::span<const SpatialRecord> records,
                                         StaticBvhConfig config = {});
    // Same identities required; membership/classification changes require Build.
    // Returns a new snapshot preserving topology, never changes existing readers.
    // Large movement may degrade pruning: rebuild to restore median partitioning.
    [[nodiscard]] StaticBvh Refit(std::uint64_t version, std::span<const SpatialRecord> records) const;

    [[nodiscard]] SceneHandle GetScene() const noexcept;
    [[nodiscard]] std::uint64_t GetVersion() const noexcept;
    [[nodiscard]] std::span<const StaticBvhNode> GetNodes() const noexcept;
    [[nodiscard]] const StaticBvhStatistics& GetStatistics() const noexcept;

    // Queries are AABB candidate tests, not geometry/narrowphase hits. Stable
    // identity order (not ray-distance order); result carries snapshot version.
    // Query coordinates are relative to origin; stored global bounds never change.
    // Inputs must be finite/bounded as SpatialBounds, except positive infinite ray max.
    [[nodiscard]] SpatialQueryResult QueryOverlap(SpatialAabb bounds, SpatialVector origin = {}) const;
    [[nodiscard]] SpatialQueryResult QueryRay(SpatialRay ray, SpatialVector origin = {}) const;
    [[nodiscard]] SpatialQueryResult QueryFrustum(const SpatialFrustum& frustum, SpatialVector origin = {}) const;

private:
    struct Data;
    explicit StaticBvh(std::shared_ptr<const Data> data);
    std::shared_ptr<const Data> m_data;
};

// Owner-thread adapter before dereferencing candidates in the live Scene. Rejects
// unloaded/foreign/recycled/disabled identities; does not pin components or verify
// that old snapshot geometry matches the current transform/source revision.
[[nodiscard]] bool IsSpatialCandidateAlive(const SceneManager& scenes, SpatialIdentity identity);

} // namespace NScene
