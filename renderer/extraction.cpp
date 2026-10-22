#include "extraction.h"
#include "render_components.h"

#include <algorithm>
#include <exception>
#include <memory>
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

NCommon::TaskHandle ScheduleRenderWorldExtraction(const NEcs::World& world,
                                                  const NResources::ResourceManager& resources,
                                                  const NEngine::NController::FrameStorage& storage,
                                                  NCommon::TaskContext& task,
                                                  std::function<void(const RenderWorld&)> completion,
                                                  std::size_t chunkSize) {
    if (storage.GetState() != NEngine::NController::EFrameState::FINALIZE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Render extraction requires an active draw frame");
    }
    if (chunkSize == 0 || !completion) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Extraction requires a chunk size and completion");
    }

    struct ViewInput {
        NEcs::Entity Entity;
        const TransformComponent* Transform;
        const CameraComponent* Camera;
    };
    struct ObjectInput {
        NEcs::Entity Entity;
        const TransformComponent* Transform;
        const RenderableComponent* Renderable;
    };
    struct Chunk {
        std::vector<RenderView> Views;
        std::vector<RenderObject> Objects;
        std::exception_ptr Error;
    };
    struct Extraction {
        std::vector<ViewInput> Views;
        std::vector<ObjectInput> Objects;
        std::vector<Chunk> Chunks;
    };

    auto extraction = std::make_shared<Extraction>();
    world.Query<TransformComponent, CameraComponent>(
            [&](NEcs::Entity entity, const TransformComponent& transform, const CameraComponent& camera) {
                extraction->Views.push_back({entity, &transform, &camera});
            });
    world.Query<TransformComponent, RenderableComponent>(
            [&](NEcs::Entity entity, const TransformComponent& transform, const RenderableComponent& renderable) {
                extraction->Objects.push_back({entity, &transform, &renderable});
            });

    const std::size_t count = std::max(extraction->Views.size(), extraction->Objects.size());
    const std::size_t chunkCount = count / chunkSize + (count % chunkSize != 0 ? 1U : 0U);
    extraction->Chunks.resize(chunkCount);
    std::vector<NCommon::TaskHandle> dependencies;
    dependencies.reserve(chunkCount + 1);
    if (chunkCount == 0) {
        dependencies.push_back(task.GetTask());
    }
    const NCommon::TaskHandle gate[] = {task.GetTask()};

    for (std::size_t index = 0; index < chunkCount; ++index) {
        dependencies.push_back(task.Spawn(
                [extraction, &resources, index, chunkSize, count](NCommon::TaskContext&) {
                    Chunk& chunk = extraction->Chunks[index];
                    try {
                        const std::size_t begin = index * chunkSize;
                        const std::size_t end = begin + std::min(chunkSize, count - begin);
                        for (std::size_t item = begin; item < end; ++item) {
                            if (item < extraction->Views.size()) {
                                const ViewInput& input = extraction->Views[item];
                                chunk.Views.push_back({MakeRenderObjectId(input.Entity),
                                                       input.Transform->Value,
                                                       input.Camera->Projection});
                            }
                            if (item < extraction->Objects.size()) {
                                const ObjectInput& input = extraction->Objects[item];
                                auto mesh = resources.TryAcquire(input.Renderable->Mesh);
                                auto material = resources.TryAcquire(input.Renderable->Material);
                                if (mesh.has_value() && material.has_value()) {
                                    chunk.Objects.push_back({MakeRenderObjectId(input.Entity),
                                                             input.Transform->Value,
                                                             std::move(*mesh),
                                                             std::move(*material)});
                                }
                            }
                        }
                    } catch (...) {
                        // All chunks must finish before the join may fail and release the frame.
                        chunk.Error = std::current_exception();
                    }
                },
                gate));
    }

    return task.Spawn(
            [extraction, storage, completion = std::move(completion)](NCommon::TaskContext&) {
                for (const Chunk& chunk: extraction->Chunks) {
                    if (chunk.Error) {
                        std::rethrow_exception(chunk.Error);
                    }
                }
                std::pmr::memory_resource& memory = storage.GetMemoryResource();
                auto& views = storage.Emplace<std::pmr::vector<RenderView>>(&memory);
                auto& objects = storage.Emplace<std::pmr::vector<RenderObject>>(&memory);
                auto& records = storage.Emplace<std::pmr::vector<NCommon::ResourceUseRecord>>(&memory);
                views.reserve(extraction->Views.size());
                objects.reserve(extraction->Objects.size());
                for (Chunk& chunk: extraction->Chunks) {
                    views.insert(views.end(), chunk.Views.begin(), chunk.Views.end());
                    for (RenderObject& object: chunk.Objects) {
                        records.push_back(object.Mesh.GetUseRecord());
                        records.push_back(object.Material.GetUseRecord());
                        objects.push_back(std::move(object));
                    }
                }
                const auto frame = storage.GetFrame();
                completion(RenderWorldBuilder::Build(memory,
                                                     {frame.GetApplicationFrameIndex(),
                                                      frame.GetSimulationIndex(),
                                                      frame.GetFrameSlotIndex(),
                                                      frame.GetGeneration()},
                                                     views,
                                                     objects,
                                                     records));
            },
            dependencies);
}

} // namespace NRenderer::NInternal
