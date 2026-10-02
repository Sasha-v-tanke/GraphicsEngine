#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <GraphicsEngine/math/camera.h>
#include <GraphicsEngine/math/transform.h>
#include <GraphicsEngine/resources/resource_lease.h>
#include <GraphicsEngine/resources/resource_use_record.h>

namespace NResources {

class Material;
class MeshData;

} // namespace NResources

namespace NRenderer {

namespace NInternal {

class RenderWorldBuilder;

} // namespace NInternal

struct RenderObjectId {
    static constexpr std::uint32_t INVALID_INDEX = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t Index = INVALID_INDEX;
    std::uint32_t Generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return Index != INVALID_INDEX;
    }

    [[nodiscard]] friend constexpr bool operator==(RenderObjectId left, RenderObjectId right) noexcept = default;
};

struct RenderFrameIdentity {
    std::uint64_t ApplicationFrameIndex = 0;
    std::uint64_t SimulationIndex = 0;
    std::size_t FrameSlotIndex = 0;
    std::uint64_t Generation = 0;

    [[nodiscard]] friend constexpr bool operator==(RenderFrameIdentity left,
                                                   RenderFrameIdentity right) noexcept = default;
};

struct RenderView {
    RenderObjectId Id;
    NMath::Transform WorldTransform;
    NMath::PerspectiveProjection Projection;
};

struct RenderObject {
    RenderObjectId Id;
    NMath::Transform WorldTransform;
    NResources::ResourceLease<NResources::MeshData> Mesh;
    NResources::ResourceLease<NResources::Material> Material;
};

class RenderWorld final {
public:
    [[nodiscard]] RenderFrameIdentity GetFrame() const noexcept {
        return m_frame;
    }

    [[nodiscard]] std::span<const RenderView> GetViews() const noexcept {
        return m_views;
    }

    [[nodiscard]] std::span<const RenderObject> GetObjects() const noexcept {
        return m_objects;
    }

    [[nodiscard]] std::span<const NResources::ResourceUseRecord> GetResourceUseRecords() const noexcept {
        return m_resourceUseRecords;
    }

private:
    RenderWorld(RenderFrameIdentity frame,
                const RenderView* views,
                std::size_t viewCount,
                const RenderObject* objects,
                std::size_t objectCount,
                const NResources::ResourceUseRecord* resourceUseRecords,
                std::size_t resourceUseRecordCount) noexcept
        : m_frame(frame)
        , m_views(views, viewCount)
        , m_objects(objects, objectCount)
        , m_resourceUseRecords(resourceUseRecords, resourceUseRecordCount) {
    }

private:
    RenderFrameIdentity m_frame;
    std::span<const RenderView> m_views;
    std::span<const RenderObject> m_objects;
    std::span<const NResources::ResourceUseRecord> m_resourceUseRecords;

    friend class NInternal::RenderWorldBuilder;
};

} // namespace NRenderer
