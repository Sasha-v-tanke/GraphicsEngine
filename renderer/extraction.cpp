#include "extraction.h"
#include "render_components.h"

#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>

namespace NRenderer::NInternal {

namespace {

[[nodiscard]] RenderObjectId MakeRenderObjectId(NEcs::Entity entity) noexcept {
    return {
            .Index = entity.Index,
            .Generation = entity.Generation,
    };
}

} // namespace

class RenderWorldBuilder final {
public:
    [[nodiscard]] static const RenderWorld&
    Build(std::pmr::memory_resource& memory,
          RenderFrameIdentity frame,
          const std::pmr::vector<RenderView>& views,
          const std::pmr::vector<RenderObject>& objects,
          const std::pmr::vector<NCommon::ResourceUseRecord>& resourceUseRecords) {
        void* storage = memory.allocate(sizeof(RenderWorld), alignof(RenderWorld));

        return *::new (storage) RenderWorld{frame,
                                            views.data(),
                                            views.size(),
                                            objects.data(),
                                            objects.size(),
                                            resourceUseRecords.data(),
                                            resourceUseRecords.size()};
    }
};

const RenderWorld& ExtractRenderWorld(const NEcs::World& world,
                                      const NResources::ResourceManager& resources,
                                      const NEngine::NController::FrameStorage& storage) {
    static_assert(std::is_trivially_destructible_v<RenderWorld>);

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
    auto& views = storage.Emplace<std::pmr::vector<RenderView>>(&memory);
    auto& objects = storage.Emplace<std::pmr::vector<RenderObject>>(&memory);
    auto& resourceUseRecords = storage.Emplace<std::pmr::vector<NCommon::ResourceUseRecord>>(&memory);

    views.reserve(viewCount);
    objects.reserve(objectCount);
    resourceUseRecords.reserve(objectCount * 2U);

    world.Query<TransformComponent, CameraComponent>(
            [&](NEcs::Entity entity, const TransformComponent& transform, const CameraComponent& camera) {
                views.push_back(RenderView{
                        .Id = MakeRenderObjectId(entity),
                        .WorldTransform = transform.Value,
                        .Projection = camera.Projection,
                });
            });

    world.Query<TransformComponent, RenderableComponent>(
            [&](NEcs::Entity entity, const TransformComponent& transform, const RenderableComponent& renderable) {
                auto mesh = resources.TryAcquire(renderable.Mesh);
                auto material = resources.TryAcquire(renderable.Material);

                if (!mesh.has_value() || !material.has_value()) {
                    return;
                }

                objects.push_back(RenderObject{
                        .Id = MakeRenderObjectId(entity),
                        .WorldTransform = transform.Value,
                        .Mesh = std::move(*mesh),
                        .Material = std::move(*material),
                });

                const RenderObject& object = objects.back();
                resourceUseRecords.push_back(object.Mesh.GetUseRecord());
                resourceUseRecords.push_back(object.Material.GetUseRecord());
            });

    const NEngine::NController::FrameHandle frame = storage.GetFrame();

    return RenderWorldBuilder::Build(memory,
                                     RenderFrameIdentity{
                                             .ApplicationFrameIndex = frame.GetApplicationFrameIndex(),
                                             .SimulationIndex = frame.GetSimulationIndex(),
                                             .FrameSlotIndex = frame.GetFrameSlotIndex(),
                                             .Generation = frame.GetGeneration(),
                                     },
                                     views,
                                     objects,
                                     resourceUseRecords);
}

} // namespace NRenderer::NInternal
