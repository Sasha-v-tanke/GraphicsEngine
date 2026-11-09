#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <GraphicsEngine/lib/common/resource_use_record.h>
#include <GraphicsEngine/math/camera.h>
#include <GraphicsEngine/math/transform.h>
#include <GraphicsEngine/renderer/render_components.h>
#include <GraphicsEngine/resources/resource_lease.h>
#include <GraphicsEngine/resources/resource_observation.h>

namespace NGraphics {

class Material;

} // namespace NGraphics

namespace NResources {

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
    NResources::ResourceLease<NGraphics::Material> Material;
};

enum class ERenderResourceRole {
    MESH,
    MATERIAL,
};

// Owned by the frame alongside RenderWorld; source state/failure never changes with a retry.
struct ResourceReadinessDiagnostic {
    RenderObjectId Object;
    ERenderResourceRole Role = ERenderResourceRole::MESH;
    EResourceReadinessPolicy Policy = EResourceReadinessPolicy::SKIP_OBJECT;
    std::size_t ResourceSlot = 0;
    std::uint64_t ResourceGeneration = 0;
    NResources::ResourceStatus Source;
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

    [[nodiscard]] std::span<const NCommon::ResourceUseRecord> GetResourceUseRecords() const noexcept {
        return m_resourceUseRecords;
    }

    [[nodiscard]] std::span<const ResourceReadinessDiagnostic> GetResourceDiagnostics() const noexcept {
        return m_resourceDiagnostics;
    }

private:
    RenderWorld(RenderFrameIdentity frame,
                const RenderView* views,
                std::size_t viewCount,
                const RenderObject* objects,
                std::size_t objectCount,
                const NCommon::ResourceUseRecord* resourceUseRecords,
                std::size_t resourceUseRecordCount,
                std::span<const ResourceReadinessDiagnostic> resourceDiagnostics) noexcept
        : m_frame(frame)
        , m_views(views, viewCount)
        , m_objects(objects, objectCount)
        , m_resourceUseRecords(resourceUseRecords, resourceUseRecordCount)
        , m_resourceDiagnostics(resourceDiagnostics) {
    }

private:
    RenderFrameIdentity m_frame;
    std::span<const RenderView> m_views;
    std::span<const RenderObject> m_objects;
    std::span<const NCommon::ResourceUseRecord> m_resourceUseRecords;
    std::span<const ResourceReadinessDiagnostic> m_resourceDiagnostics;

    friend class NInternal::RenderWorldBuilder;
};

} // namespace NRenderer
