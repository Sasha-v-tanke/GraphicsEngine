#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <tuple>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>
#include <GraphicsEngine/scene/static_bvh.h>

namespace NScene::NSpatialQueryDetail {

inline void Require(bool condition, const char* message) {
    if (!condition) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "{}", message);
    }
}
inline std::array<double, 3> Values(SpatialVector vector) {
    return {vector.X, vector.Y, vector.Z};
}
inline SpatialVector Vector(std::array<double, 3> values) {
    return {values[0], values[1], values[2]};
}
inline bool IsCoordinate(double value) {
    return std::isfinite(value) && std::abs(value) <= SpatialBounds::MAX_COORDINATE;
}
inline bool IsVector(SpatialVector vector) {
    return IsCoordinate(vector.X) && IsCoordinate(vector.Y) && IsCoordinate(vector.Z);
}
inline bool IsBox(SpatialAabb box) {
    return IsVector(box.Min) && IsVector(box.Max) && box.Min.X <= box.Max.X && box.Min.Y <= box.Max.Y &&
           box.Min.Z <= box.Max.Z;
}
inline auto Key(SpatialIdentity identity) {
    return std::tuple{identity.Entity.WorldId, identity.Entity.Identity.Index, identity.Entity.Identity.Generation};
}
inline SpatialAabb Union(SpatialAabb left, SpatialAabb right) {
    return {{std::min(left.Min.X, right.Min.X), std::min(left.Min.Y, right.Min.Y), std::min(left.Min.Z, right.Min.Z)},
            {std::max(left.Max.X, right.Max.X), std::max(left.Max.Y, right.Max.Y), std::max(left.Max.Z, right.Max.Z)}};
}
inline bool Overlaps(SpatialAabb left, SpatialAabb right) {
    return left.Min.X <= right.Max.X && right.Min.X <= left.Max.X && left.Min.Y <= right.Max.Y &&
           right.Min.Y <= left.Max.Y && left.Min.Z <= right.Max.Z && right.Min.Z <= left.Max.Z;
}
inline constexpr double DOWN = -std::numeric_limits<double>::infinity();
inline constexpr double UP = std::numeric_limits<double>::infinity();
inline SpatialAabb GlobalBox(SpatialAabb relative, SpatialVector origin) {
    const auto lo = Values(relative.Min);
    const auto hi = Values(relative.Max);
    const auto offset = Values(origin);
    std::array<double, 3> min{};
    std::array<double, 3> max{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        min[axis] = std::nextafter(lo[axis] + offset[axis], DOWN);
        max[axis] = std::nextafter(hi[axis] + offset[axis], UP);
    }
    return {Vector(min), Vector(max)};
}

inline bool RayHits(SpatialAabb box, const SpatialRay& ray, SpatialVector offset) {
    const auto min = Values(box.Min);
    const auto max = Values(box.Max);
    const auto relative = Values(ray.Origin);
    const auto origin = Values(offset);
    const auto direction = Values(ray.Direction);
    double near = ray.MinDistance;
    double far = ray.MaxDistance;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        // Subtract origin in two outward-rounded steps; do not round a small
        // camera-relative displacement into a large global point first.
        const double lo = std::nextafter(std::nextafter(min[axis] - origin[axis], DOWN) - relative[axis], DOWN);
        const double hi = std::nextafter(std::nextafter(max[axis] - origin[axis], UP) - relative[axis], UP);
        if (direction[axis] == 0) {
            if (lo > 0 || hi < 0) {
                return false;
            }
            continue;
        }
        const double first = std::nextafter((direction[axis] > 0 ? lo : hi) / direction[axis], DOWN);
        const double last = std::nextafter((direction[axis] > 0 ? hi : lo) / direction[axis], UP);
        near = std::max(near, first);
        far = std::min(far, last);
        if (near > far) {
            return false;
        }
    }
    return true;
}

inline bool FrustumHits(SpatialAabb box, const SpatialFrustum& frustum, SpatialVector origin) {
    const auto min = Values(box.Min);
    const auto max = Values(box.Max);
    const auto offset = Values(origin);
    for (const SpatialPlane& plane: frustum.Planes) {
        const auto normal = Values(plane.Normal);
        double upper = plane.Offset;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double point = normal[axis] >= 0 ? max[axis] : min[axis];
            const double relative = std::nextafter(point - offset[axis], normal[axis] >= 0 ? UP : DOWN);
            upper = std::nextafter(upper + std::nextafter(normal[axis] * relative, UP), UP);
        }
        if (upper < 0) {
            return false;
        }
    }
    return true;
}

} // namespace NScene::NSpatialQueryDetail
