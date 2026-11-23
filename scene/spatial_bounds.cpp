#include "spatial_bounds.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <GraphicsEngine/scene/scene_manager.h>
#include <ecs/internal/adaptive_traversal.h>

namespace NScene {
namespace {

using Coordinates = std::array<double, 3>;

Coordinates Values(SpatialVector value) {
    return {value.X, value.Y, value.Z};
}
SpatialVector Vector(Coordinates value) {
    return {value[0], value[1], value[2]};
}
bool IsCoordinate(double value) {
    return std::isfinite(value) && std::abs(value) <= SpatialBounds::MAX_COORDINATE;
}
bool IsAabb(SpatialAabb bounds) {
    const auto min = Values(bounds.Min);
    const auto max = Values(bounds.Max);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!IsCoordinate(min[axis]) || !IsCoordinate(max[axis]) || min[axis] > max[axis]) {
            return false;
        }
    }
    return true;
}

SpatialRecord
Capture(const SceneManager& scenes, SpatialIdentity identity, const SpatialBoundsComponent* source, bool useComponent) {
    SpatialRecord record;
    record.Identity = identity;
    if (!scenes.IsRegistered(identity.Scene)) {
        return record;
    }
    const auto scene = scenes.Acquire(identity.Scene);
    const auto& world = std::as_const(scene->GetWorld());
    const auto topology = world.AcquireTopologySnapshot();
    if (identity.Entity.WorldId != topology.GetWorldId()) {
        record.Validity = ESpatialValidity::FOREIGN_ENTITY;
        return record;
    }
    if (!topology.IsAlive(identity.Entity)) {
        record.Validity = ESpatialValidity::STALE_ENTITY;
        return record;
    }
    if (!world.IsEntityEnabled(identity.Entity)) {
        record.Validity = ESpatialValidity::DISABLED;
        return record;
    }
    if (useComponent) {
        if (!world.HasComponent<SpatialBoundsComponent>(identity.Entity.Identity)) {
            record.Validity = ESpatialValidity::MISSING_BOUNDS;
            return record;
        }
        if (!world.IsComponentEnabled<SpatialBoundsComponent>(identity.Entity)) {
            record.Validity = ESpatialValidity::DISABLED;
            return record;
        }
        source = &world.GetComponent<SpatialBoundsComponent>(identity.Entity.Identity);
    }
    record.SourceRevision = source->Revision;
    record.Mobility = source->Mobility;
    const auto& state = NEcs::NInternal::TraversalTopologyAccess::Get(topology);
    for (auto entity = identity.Entity; entity != NEcs::EntityReference{}; entity = topology.GetParent(entity)) {
        record.TransformRevisions.push_back({entity, state.Get(entity).Local.LocalRevision});
    }
    if (source->Revision != 0 && (!source->Mobility || *source->Mobility == ESpatialMobility::STATIC ||
                                  *source->Mobility == ESpatialMobility::DYNAMIC)) {
        record.Bounds = TransformSpatialBounds(source->LocalBounds, topology.GetWorldTransform(identity.Entity));
    }
    record.Validity = record.Bounds ? ESpatialValidity::VALID : ESpatialValidity::INVALID_BOUNDS;
    return record;
}

} // namespace

std::optional<SpatialBounds> TransformSpatialBounds(const std::variant<SpatialAabb, SpatialSphere>& local,
                                                    const NMath::Mat4& globalTransform) {
    for (float value: globalTransform.GetValues()) {
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
    }
    if (globalTransform(3, 0) != 0 || globalTransform(3, 1) != 0 || globalTransform(3, 2) != 0 ||
        globalTransform(3, 3) != 1) {
        return std::nullopt;
    }
    SpatialAabb input;
    if (const auto* box = std::get_if<SpatialAabb>(&local)) {
        input = *box;
    } else {
        const auto& sphere = std::get<SpatialSphere>(local);
        const auto center = Values(sphere.Center);
        if (!std::ranges::all_of(center, IsCoordinate) || !IsCoordinate(sphere.Radius) || sphere.Radius < 0) {
            return std::nullopt;
        }
        Coordinates min{};
        Coordinates max{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            min[axis] = std::nextafter(center[axis] - sphere.Radius, -std::numeric_limits<double>::infinity());
            max[axis] = std::nextafter(center[axis] + sphere.Radius, std::numeric_limits<double>::infinity());
        }
        input = {Vector(min), Vector(max)};
    }
    if (!IsAabb(input)) {
        return std::nullopt;
    }
    const auto inputMin = Values(input.Min);
    const auto inputMax = Values(input.Max);
    Coordinates min{};
    Coordinates max{};
    Coordinates transformedCenter{};
    for (std::size_t row = 0; row < 3; ++row) {
        min[row] = globalTransform(row, 3);
        max[row] = globalTransform(row, 3);
        for (std::size_t column = 0; column < 3; ++column) {
            const double factor = globalTransform(row, column);
            const double lower = factor * (factor >= 0 ? inputMin[column] : inputMax[column]);
            const double upper = factor * (factor >= 0 ? inputMax[column] : inputMin[column]);
            // Round each product and sum outwards, including cancellation cases.
            min[row] = std::nextafter(min[row] + std::nextafter(lower, -std::numeric_limits<double>::infinity()),
                                      -std::numeric_limits<double>::infinity());
            max[row] = std::nextafter(max[row] + std::nextafter(upper, std::numeric_limits<double>::infinity()),
                                      std::numeric_limits<double>::infinity());
        }
        transformedCenter[row] = (min[row] + max[row]) * 0.5;
    }
    SpatialBounds result;
    result.Aabb = {Vector(min), Vector(max)};
    if (!IsAabb(result.Aabb)) {
        return std::nullopt;
    }
    result.Sphere.Center = Vector(transformedCenter);
    Coordinates radiusExtent{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        radiusExtent[axis] = std::nextafter(
                std::max(std::abs(min[axis] - transformedCenter[axis]), std::abs(max[axis] - transformedCenter[axis])),
                std::numeric_limits<double>::infinity());
    }
    result.Sphere.Radius = std::nextafter(std::hypot(radiusExtent[0], radiusExtent[1], radiusExtent[2]),
                                          std::numeric_limits<double>::infinity());
    if (!IsCoordinate(result.Sphere.Radius)) {
        return std::nullopt;
    }
    return result;
}

SpatialRecord CaptureSpatialRecord(const SceneManager& scenes, SpatialIdentity identity) {
    return Capture(scenes, identity, nullptr, true);
}
SpatialRecord
CaptureSpatialRecord(const SceneManager& scenes, SpatialIdentity identity, const SpatialBoundsComponent& source) {
    return Capture(scenes, identity, &source, false);
}
std::vector<SpatialRecord> CaptureSpatialRecords(const SceneManager& scenes, SceneHandle scene) {
    // Acquire once for lifetime; each record independently validates the scene-qualified identity.
    const auto lease = scenes.Acquire(scene);
    const auto& world = std::as_const(lease->GetWorld());
    std::vector<SpatialRecord> records;
    world.Query<SpatialBoundsComponent>([&](NEcs::Entity entity, const SpatialBoundsComponent&) {
        const auto identity = world.GetEntityReference(entity);
        if (world.IsEntityEnabled(identity) && world.IsComponentEnabled<SpatialBoundsComponent>(identity)) {
            records.push_back(CaptureSpatialRecord(scenes, {scene, identity}));
        }
    });
    return records;
}

} // namespace NScene
