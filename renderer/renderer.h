#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>

#include <GraphicsEngine/math/transform.h>
#include <GraphicsEngine/renderer/render_world.h>
#include <GraphicsEngine/resources/resource_handle.h>

namespace NGraphics {

class Material;

} // namespace NGraphics

namespace NResources {

class MeshData;

} // namespace NResources

namespace NRenderer {

struct DrawCommandData {
    RenderObjectId ViewId;
    RenderObjectId ObjectId;
    NMath::Transform WorldTransform;
    NResources::ResourceHandle<NResources::MeshData> Mesh;
    std::uint64_t MeshVersion = 0;
    NResources::ResourceHandle<NGraphics::Material> Material;
    std::uint64_t MaterialVersion = 0;
};

class RenderPlan final {
public:
    [[nodiscard]] RenderFrameIdentity GetFrame() const noexcept {
        return m_frame;
    }

    [[nodiscard]] std::span<const DrawCommandData> GetDrawCommands() const noexcept {
        return m_drawCommands;
    }

private:
    RenderPlan(RenderFrameIdentity frame, const DrawCommandData* drawCommands, std::size_t drawCommandCount) noexcept
        : m_frame(frame)
        , m_drawCommands(drawCommands, drawCommandCount) {
    }

private:
    RenderFrameIdentity m_frame;
    std::span<const DrawCommandData> m_drawCommands;

    friend class Renderer;
};

class Renderer final {
public:
    [[nodiscard]] static const RenderPlan& Prepare(const RenderWorld& world, std::pmr::memory_resource& memory);
};

} // namespace NRenderer
