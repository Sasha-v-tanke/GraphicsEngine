#include "static_bvh.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <tuple>
#include <utility>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>
#include <GraphicsEngine/scene/scene_manager.h>

namespace NScene {
namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "{}", message);
    }
}
std::array<double, 3> Values(SpatialVector vector) {
    return {vector.X, vector.Y, vector.Z};
}
SpatialVector Vector(std::array<double, 3> values) {
    return {values[0], values[1], values[2]};
}
bool IsCoordinate(double value) {
    return std::isfinite(value) && std::abs(value) <= SpatialBounds::MAX_COORDINATE;
}
bool IsVector(SpatialVector vector) {
    return IsCoordinate(vector.X) && IsCoordinate(vector.Y) && IsCoordinate(vector.Z);
}
bool IsBox(SpatialAabb box) {
    return IsVector(box.Min) && IsVector(box.Max) && box.Min.X <= box.Max.X && box.Min.Y <= box.Max.Y &&
           box.Min.Z <= box.Max.Z;
}
auto Key(SpatialIdentity identity) {
    return std::tuple{identity.Entity.WorldId, identity.Entity.Identity.Index, identity.Entity.Identity.Generation};
}
SpatialAabb Union(SpatialAabb left, SpatialAabb right) {
    return {{std::min(left.Min.X, right.Min.X), std::min(left.Min.Y, right.Min.Y), std::min(left.Min.Z, right.Min.Z)},
            {std::max(left.Max.X, right.Max.X), std::max(left.Max.Y, right.Max.Y), std::max(left.Max.Z, right.Max.Z)}};
}
bool Overlaps(SpatialAabb left, SpatialAabb right) {
    return left.Min.X <= right.Max.X && right.Min.X <= left.Max.X && left.Min.Y <= right.Max.Y &&
           right.Min.Y <= left.Max.Y && left.Min.Z <= right.Max.Z && right.Min.Z <= left.Max.Z;
}
constexpr double DOWN = -std::numeric_limits<double>::infinity();
constexpr double UP = std::numeric_limits<double>::infinity();
SpatialAabb GlobalBox(SpatialAabb relative, SpatialVector origin) {
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

struct Entry {
    SpatialIdentity Identity;
    SpatialAabb Bounds;
};

std::vector<Entry> Entries(SceneHandle scene, std::span<const SpatialRecord> records, StaticBvhConfig config) {
    Require(records.size() <= config.MaxRecords, "Static BVH input budget exceeded");
    std::vector<Entry> entries;
    entries.reserve(records.size());
    std::uint64_t worldId = 0;
    for (const SpatialRecord& record: records) {
        if (record.Validity != ESpatialValidity::VALID || record.Mobility != ESpatialMobility::STATIC) {
            continue;
        }
        Require(record.Identity.Scene == scene && record.Identity.Entity.WorldId != 0 &&
                        record.Identity.Entity.Identity.IsValid() && record.SourceRevision != 0 && record.Bounds &&
                        IsBox(record.Bounds->Aabb),
                "Malformed static spatial record");
        Require(worldId == 0 || worldId == record.Identity.Entity.WorldId, "Mixed World identities in static BVH");
        worldId = record.Identity.Entity.WorldId;
        entries.push_back({record.Identity, record.Bounds->Aabb});
    }
    std::ranges::sort(entries,
                      [](const Entry& left, const Entry& right) { return Key(left.Identity) < Key(right.Identity); });
    for (std::size_t index = 1; index < entries.size(); ++index) {
        Require(entries[index - 1].Identity != entries[index].Identity, "Duplicate static spatial identity");
    }
    return entries;
}

std::uint32_t BuildNode(auto& data, std::uint32_t begin, std::uint32_t count, std::size_t depth) {
    const auto index = static_cast<std::uint32_t>(data.Nodes.size());
    data.Nodes.push_back({});
    data.Statistics.MaxDepth = std::max(data.Statistics.MaxDepth, depth);
    SpatialAabb bounds = data.Entries[data.Order[begin]].Bounds;
    for (std::uint32_t item = 1; item < count; ++item) {
        bounds = Union(bounds, data.Entries[data.Order[begin + item]].Bounds);
    }
    data.Nodes[index].Bounds = bounds;
    if (count <= data.Config.LeafCapacity) {
        data.Nodes[index].First = begin;
        data.Nodes[index].Count = count;
        std::sort(data.Order.begin() + begin, data.Order.begin() + begin + count);
        ++data.Statistics.Leaves;
        return index;
    }
    const auto min = Values(bounds.Min);
    const auto max = Values(bounds.Max);
    std::size_t axis = 0;
    for (std::size_t candidate = 1; candidate < 3; ++candidate) {
        if (max[candidate] - min[candidate] > max[axis] - min[axis]) {
            axis = candidate;
        }
    }
    const auto split = begin + count / 2;
    std::nth_element(data.Order.begin() + begin,
                     data.Order.begin() + split,
                     data.Order.begin() + begin + count,
                     [&](std::uint32_t left, std::uint32_t right) {
                         const auto& a = data.Entries[left];
                         const auto& b = data.Entries[right];
                         const double ac = Values(a.Bounds.Min)[axis] + Values(a.Bounds.Max)[axis];
                         const double bc = Values(b.Bounds.Min)[axis] + Values(b.Bounds.Max)[axis];
                         return ac != bc ? ac < bc : left < right;
                     });
    const auto left = BuildNode(data, begin, count / 2, depth + 1);
    const auto right = BuildNode(data, split, count - count / 2, depth + 1);
    data.Nodes[index].Left = left;
    data.Nodes[index].Right = right;
    return index;
}

void FinishStatistics(auto& data, std::size_t inputs, std::uint64_t version) {
    data.Statistics.Version = version;
    data.Statistics.InputRecords = inputs;
    data.Statistics.IndexedRecords = data.Entries.size();
    data.Statistics.SkippedRecords = inputs - data.Entries.size();
    data.Statistics.Nodes = data.Nodes.size();
    data.Statistics.StorageBytes = sizeof(data) + data.Entries.capacity() * sizeof(Entry) +
                                   data.Order.capacity() * sizeof(std::uint32_t) +
                                   data.Nodes.capacity() * sizeof(StaticBvhNode);
}

SpatialQueryResult Query(const auto& data, const auto& matches) {
    SpatialQueryResult result;
    result.Version = data.Statistics.Version;
    if (data.Nodes.empty()) {
        return result;
    }
    // Median splitting and the uint32 input budget bound pending siblings to <= 32.
    std::array<std::uint32_t, 64> stack{};
    std::size_t size = 1;
    while (size != 0) {
        const auto& node = data.Nodes[stack[--size]];
        ++result.Statistics.VisitedNodes;
        if (!matches(node.Bounds)) {
            continue;
        }
        if (node.Count == 0) {
            stack[size++] = node.Right;
            stack[size++] = node.Left;
            continue;
        }
        for (std::uint32_t item = 0; item < node.Count; ++item) {
            const auto& entry = data.Entries[data.Order[node.First + item]];
            ++result.Statistics.TestedItems;
            if (matches(entry.Bounds)) {
                result.Candidates.push_back(entry.Identity);
            }
        }
    }
    std::ranges::sort(result.Candidates,
                      [](SpatialIdentity left, SpatialIdentity right) { return Key(left) < Key(right); });
    result.Statistics.MatchedItems = result.Candidates.size();
    return result;
}

bool RayHits(SpatialAabb box, const SpatialRay& ray, SpatialVector offset) {
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

bool FrustumHits(SpatialAabb box, const SpatialFrustum& frustum, SpatialVector origin) {
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

} // namespace

struct StaticBvh::Data {
    SceneHandle Scene;
    StaticBvhConfig Config;
    std::vector<Entry> Entries;
    std::vector<std::uint32_t> Order;
    std::vector<StaticBvhNode> Nodes;
    StaticBvhStatistics Statistics;
};

StaticBvh::StaticBvh(std::shared_ptr<const Data> data)
    : m_data(std::move(data)) {
}
StaticBvh StaticBvh::Build(SceneHandle scene,
                           std::uint64_t version,
                           std::span<const SpatialRecord> records,
                           StaticBvhConfig config) {
    Require(scene.IsValid() && version != 0 && config.MaxRecords != 0 &&
                    config.MaxRecords <= std::numeric_limits<std::uint32_t>::max() / 2 && config.LeafCapacity != 0 &&
                    config.LeafCapacity <= 64,
            "Invalid static BVH configuration");
    auto data = std::make_shared<Data>();
    data->Scene = scene;
    data->Config = config;
    data->Entries = Entries(scene, records, config);
    data->Order.reserve(data->Entries.size());
    for (std::size_t item = 0; item < data->Entries.size(); ++item) {
        data->Order.push_back(static_cast<std::uint32_t>(item));
    }
    if (!data->Entries.empty()) {
        data->Nodes.reserve(std::min(data->Entries.size() * 2 - 1, data->Entries.size() * 4 / config.LeafCapacity + 1));
        (void)BuildNode(*data, 0, static_cast<std::uint32_t>(data->Entries.size()), 1);
    }
    FinishStatistics(*data, records.size(), version);
    return StaticBvh{std::move(data)};
}
StaticBvh StaticBvh::Refit(std::uint64_t version, std::span<const SpatialRecord> records) const {
    Require(version > GetVersion(), "Refit requires a newer snapshot version");
    auto data = std::make_shared<Data>();
    data->Scene = m_data->Scene;
    data->Config = m_data->Config;
    data->Entries = Entries(data->Scene, records, data->Config);
    Require(data->Entries.size() == m_data->Entries.size(), "Static BVH membership changed; rebuild required");
    for (std::size_t item = 0; item < data->Entries.size(); ++item) {
        Require(data->Entries[item].Identity == m_data->Entries[item].Identity,
                "Static BVH identity changed; rebuild required");
    }
    data->Order = m_data->Order;
    data->Nodes = m_data->Nodes;
    data->Statistics.Leaves = m_data->Statistics.Leaves;
    data->Statistics.MaxDepth = m_data->Statistics.MaxDepth;
    for (std::size_t cursor = data->Nodes.size(); cursor != 0; --cursor) {
        auto& node = data->Nodes[cursor - 1];
        if (node.Count == 0) {
            node.Bounds = Union(data->Nodes[node.Left].Bounds, data->Nodes[node.Right].Bounds);
        } else {
            node.Bounds = data->Entries[data->Order[node.First]].Bounds;
            for (std::uint32_t item = 1; item < node.Count; ++item) {
                node.Bounds = Union(node.Bounds, data->Entries[data->Order[node.First + item]].Bounds);
            }
        }
    }
    FinishStatistics(*data, records.size(), version);
    return StaticBvh{std::move(data)};
}
SceneHandle StaticBvh::GetScene() const noexcept {
    return m_data->Scene;
}
std::uint64_t StaticBvh::GetVersion() const noexcept {
    return m_data->Statistics.Version;
}
std::span<const StaticBvhNode> StaticBvh::GetNodes() const noexcept {
    return m_data->Nodes;
}
const StaticBvhStatistics& StaticBvh::GetStatistics() const noexcept {
    return m_data->Statistics;
}

SpatialQueryResult StaticBvh::QueryOverlap(SpatialAabb bounds, SpatialVector origin) const {
    Require(IsBox(bounds) && IsVector(origin), "Invalid static BVH overlap query");
    const auto global = GlobalBox(bounds, origin);
    return Query(*m_data, [&](SpatialAabb box) { return Overlaps(box, global); });
}
SpatialQueryResult StaticBvh::QueryRay(SpatialRay ray, SpatialVector origin) const {
    Require(IsVector(ray.Origin) && IsVector(ray.Direction) && IsVector(origin) && ray.Direction != SpatialVector{} &&
                    std::isfinite(ray.MinDistance) && ray.MinDistance >= 0 && !std::isnan(ray.MaxDistance) &&
                    ray.MaxDistance >= ray.MinDistance,
            "Invalid static BVH ray query");
    return Query(*m_data, [&](SpatialAabb box) { return RayHits(box, ray, origin); });
}
SpatialQueryResult StaticBvh::QueryFrustum(const SpatialFrustum& frustum, SpatialVector origin) const {
    Require(IsVector(origin), "Invalid static BVH query origin");
    for (const SpatialPlane& plane: frustum.Planes) {
        Require(IsVector(plane.Normal) && plane.Normal != SpatialVector{} && IsCoordinate(plane.Offset),
                "Invalid static BVH frustum plane");
    }
    return Query(*m_data, [&](SpatialAabb box) { return FrustumHits(box, frustum, origin); });
}
bool IsSpatialCandidateAlive(const SceneManager& scenes, SpatialIdentity identity) {
    if (!scenes.IsRegistered(identity.Scene)) {
        return false;
    }
    const auto lease = scenes.Acquire(identity.Scene);
    const auto& world = std::as_const(lease->GetWorld());
    const auto topology = world.AcquireTopologySnapshot();
    return topology.IsAlive(identity.Entity) && world.IsEntityEnabled(identity.Entity);
}
std::string StaticBvhStatistics::Serialize() const {
    return std::format("{{\"schema\":1,\"version\":{},\"input\":{},\"indexed\":{},\"skipped\":{},\"nodes\":{},"
                       "\"leaves\":{},\"max_depth\":{},\"storage_bytes\":{}}}",
                       Version,
                       InputRecords,
                       IndexedRecords,
                       SkippedRecords,
                       Nodes,
                       Leaves,
                       MaxDepth,
                       StorageBytes);
}
std::string SpatialQueryStatistics::Serialize() const {
    return std::format(R"({{"schema":1,"visited_nodes":{},"tested_items":{},"matched_items":{}}})",
                       VisitedNodes,
                       TestedItems,
                       MatchedItems);
}

} // namespace NScene
