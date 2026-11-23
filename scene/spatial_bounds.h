#pragma once

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include <GraphicsEngine/ecs/hierarchy.h>
#include <GraphicsEngine/lib/common/change_tracker.h>
#include <GraphicsEngine/math/matrix.h>
#include <GraphicsEngine/scene/scene_handle.h>

namespace NScene {

class SceneManager;

// Double precision global scene coordinates; rendering-origin offsets are not applied.
struct SpatialVector {
    double X = 0;
    double Y = 0;
    double Z = 0;
    friend bool operator==(SpatialVector, SpatialVector) = default;
};

struct SpatialAabb {
    SpatialVector Min;
    SpatialVector Max;
    friend bool operator==(SpatialAabb, SpatialAabb) = default;
};

struct SpatialSphere {
    SpatialVector Center;
    double Radius = 0;
    friend bool operator==(SpatialSphere, SpatialSphere) = default;
};

struct SpatialBounds {
    // Explicit admission limit for inputs and transformed outputs, in scene units.
    static constexpr double MAX_COORDINATE = 1.0e12;
    SpatialAabb Aabb;
    SpatialSphere Sphere;
    friend bool operator==(SpatialBounds, SpatialBounds) = default;
};

enum class ESpatialMobility {
    STATIC,
    DYNAMIC,
};

// Value-owned provider result. Revisions are nonzero and scoped to this entity's
// source. Change Revision whenever bounds or classification changes. Removing/adding
// this component publishes at the existing ECS commits (separate epochs; the
// World rejects conflicting remove/add commands for one component in an epoch).
struct SpatialBoundsComponent {
    std::variant<SpatialAabb, SpatialSphere> LocalBounds = SpatialAabb{};
    std::optional<ESpatialMobility> Mobility;
    std::uint64_t Revision = 1;
};

struct SpatialIdentity {
    SceneHandle Scene;
    NEcs::EntityReference Entity;
    friend bool operator==(SpatialIdentity, SpatialIdentity) = default;
};

enum class ESpatialValidity {
    VALID,
    SCENE_UNAVAILABLE,
    FOREIGN_ENTITY,
    STALE_ENTITY,
    DISABLED,
    MISSING_BOUNDS,
    INVALID_BOUNDS,
};

struct SpatialTransformRevision {
    NEcs::EntityReference Entity;
    NCommon::VersionStamp Local;
    friend bool operator==(SpatialTransformRevision, SpatialTransformRevision) = default;
};

struct SpatialRecord {
    SpatialIdentity Identity;
    ESpatialValidity Validity = ESpatialValidity::SCENE_UNAVAILABLE;
    std::optional<SpatialBounds> Bounds;
    std::optional<ESpatialMobility> Mobility;
    std::uint64_t SourceRevision = 0;
    // Exact child-to-root identity/revision chain, without hash collisions or a
    // second transform cache. Parent edits/reparenting invalidate this token.
    std::vector<SpatialTransformRevision> TransformRevisions;
};

// Pure affine transform. Returns nullopt for non-finite, inverted, projective or
// out-of-budget bounds/matrices. AABB and sphere conservatively enclose the shape,
// including rotation, negative/nonuniform scales and hierarchy shear.
[[nodiscard]] std::optional<SpatialBounds> TransformSpatialBounds(const std::variant<SpatialAabb, SpatialSphere>& local,
                                                                  const NMath::Mat4& globalTransform);

// Owner-thread/quiescent capture only, after producers retire and ECS commit.
// Does not commit, schedule or wait. Pending changes remain invisible. Scene/World
// must not mutate during capture. Exceptions from access/allocation propagate;
// no partial snapshot is published. Results own values, never World/component
// pointers or leases, and remain immutable inputs for downstream spatial indexes.
[[nodiscard]] SpatialRecord CaptureSpatialRecord(const SceneManager& scenes, SpatialIdentity identity);
// Custom bounds-provider adapter; source is copied synchronously, never retained.
[[nodiscard]] SpatialRecord
CaptureSpatialRecord(const SceneManager& scenes, SpatialIdentity identity, const SpatialBoundsComponent& source);
// Enabled component adapter, deterministic entity order; invalid inputs retain
// diagnostics. Missing/disabled components do not enter this vector. Single-record
// capture above exposes their explicit validity states when identity is known.
[[nodiscard]] std::vector<SpatialRecord> CaptureSpatialRecords(const SceneManager& scenes, SceneHandle scene);

} // namespace NScene
