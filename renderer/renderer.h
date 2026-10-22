#pragma once

#include <cstdint>
#include <functional>
#include <memory_resource>
#include <span>

#include <GraphicsEngine/lib/thread/task/task_system.h>
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

// A contiguous run of compatible instances in the unchanged direct-command stream.
struct RenderBatch {
    std::size_t FirstCommand = 0;
    std::size_t CommandCount = 0;
};

class RenderPlan final {
public:
    [[nodiscard]] RenderFrameIdentity GetFrame() const noexcept {
        return m_frame;
    }

    [[nodiscard]] std::span<const DrawCommandData> GetDrawCommands() const noexcept {
        return m_drawCommands;
    }

    [[nodiscard]] std::span<const RenderBatch> GetBatches() const noexcept {
        return m_batches;
    }

private:
    RenderPlan(RenderFrameIdentity frame,
               std::span<const DrawCommandData> drawCommands,
               std::span<const RenderBatch> batches) noexcept
        : m_frame(frame)
        , m_drawCommands(drawCommands)
        , m_batches(batches) {
    }

private:
    RenderFrameIdentity m_frame;
    std::span<const DrawCommandData> m_drawCommands;
    std::span<const RenderBatch> m_batches;

    friend class Renderer;
};

class Renderer final {
public:
    [[nodiscard]] static const RenderPlan& Prepare(const RenderWorld& world, std::pmr::memory_resource& memory);

    // Snapshot, arena and completion captures must outlive the returned join. All work is gated on task.GetTask().
    [[nodiscard]] static NCommon::TaskHandle SchedulePrepare(const RenderWorld& world,
                                                             std::pmr::memory_resource& memory,
                                                             NCommon::TaskContext& task,
                                                             std::function<void(const RenderPlan&)> completion,
                                                             std::size_t chunkSize = 4096);

private:
    [[nodiscard]] static const RenderPlan& Publish(RenderFrameIdentity frame,
                                                   std::span<const DrawCommandData> sortedCommands,
                                                   std::pmr::memory_resource& memory);
};

} // namespace NRenderer
