#include "extraction.h"

#include "render_components.h"

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <type_traits>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>

namespace NRenderer::NInternal {

namespace {

template<typename T>
[[nodiscard]] T* AllocateArray(std::pmr::memory_resource& memory, std::size_t count) {
    if (count == 0) {
        return nullptr;
    }

    return static_cast<T*>(memory.allocate(sizeof(T) * count, alignof(T)));
}

[[nodiscard]] RenderObjectId MakeRenderObjectId(NEcs::Entity entity) noexcept {
    return {
            .Index = entity.Index,
            .Generation = entity.Generation,
    };
}

} // namespace

class RenderWorldBuilder final {
public:
    [[nodiscard]] static RenderWorld Build(RenderFrameIdentity frame,
                                           const RenderView* views,
                                           std::size_t viewCount,
                                           const RenderObject* objects,
                                           std::size_t objectCount) noexcept {
        return RenderWorld{frame, views, viewCount, objects, objectCount};
    }
};

RenderWorld ExtractRenderWorld(const NEcs::World& world, const NEngine::NController::FrameStorage& storage) {
    static_assert(std::is_trivially_destructible_v<RenderView>);
    static_assert(std::is_trivially_destructible_v<RenderObject>);

    if (storage.GetState() != NEngine::NController::EFrameState::FINALIZE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Render extraction requires an active draw frame");
    }

    std::size_t viewCount = 0;
    std::size_t objectCount = 0;

    world.Query<TransformComponent, CameraComponent>(
            [&](NEcs::Entity, const TransformComponent&, const CameraComponent&) { ++viewCount; });
    world.Query<TransformComponent, RenderableComponent>(
            [&](NEcs::Entity, const TransformComponent&, const RenderableComponent&) { ++objectCount; });

    std::pmr::memory_resource& memory = storage.GetMemoryResource();
    RenderView* views = AllocateArray<RenderView>(memory, viewCount);
    RenderObject* objects = AllocateArray<RenderObject>(memory, objectCount);

    std::size_t viewIndex = 0;
    world.Query<TransformComponent, CameraComponent>(
            [&](NEcs::Entity entity, const TransformComponent& transform, const CameraComponent& camera) {
                std::construct_at(&views[viewIndex],
                                  RenderView{
                                          .Id = MakeRenderObjectId(entity),
                                          .WorldTransform = transform.Value,
                                          .Projection = camera.Projection,
                                  });
                ++viewIndex;
            });

    std::size_t objectIndex = 0;
    world.Query<TransformComponent, RenderableComponent>(
            [&](NEcs::Entity entity, const TransformComponent& transform, const RenderableComponent& renderable) {
                std::construct_at(&objects[objectIndex],
                                  RenderObject{
                                          .Id = MakeRenderObjectId(entity),
                                          .WorldTransform = transform.Value,
                                          .Mesh = renderable.Mesh,
                                          .Material = renderable.Material,
                                  });
                ++objectIndex;
            });

    const NEngine::NController::FrameHandle frame = storage.GetFrame();

    return RenderWorldBuilder::Build(
            RenderFrameIdentity{
                    .ApplicationFrameIndex = frame.GetApplicationFrameIndex(),
                    .SimulationIndex = frame.GetSimulationIndex(),
                    .FrameSlotIndex = frame.GetFrameSlotIndex(),
                    .Generation = frame.GetGeneration(),
            },
            views,
            viewCount,
            objects,
            objectCount);
}

} // namespace NRenderer::NInternal
