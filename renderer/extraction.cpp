#include "extraction.h"
#include "render_components.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include <GraphicsEngine/graphics/material.h>
#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>
#include <GraphicsEngine/resources/mesh_data.h>

namespace NRenderer::NInternal {

namespace {

[[nodiscard]] RenderObjectId MakeRenderObjectId(NEcs::Entity entity) noexcept {
    return {
            .Index = entity.Index,
            .Generation = entity.Generation,
    };
}

struct CapturedResources {
    NResources::ResourceHandle<NResources::MeshData> MeshHandle;
    NResources::ResourceHandle<NGraphics::Material> MaterialHandle;
    EResourceReadinessPolicy Policy = EResourceReadinessPolicy::SKIP_OBJECT;
    NResources::ResourceObservation<NResources::MeshData> Mesh;
    NResources::ResourceObservation<NGraphics::Material> Material;
};

CapturedResources CaptureResources(const NResources::ResourceManager& resources, const RenderableComponent& input) {
    return {input.Mesh,
            input.Material,
            input.ReadinessPolicy,
            resources.Observe(input.Mesh),
            resources.Observe(input.Material)};
}

template<typename T>
void ValidateReference(RenderObjectId object,
                       const char* role,
                       NResources::ResourceHandle<T> handle,
                       const NResources::ResourceStatus& status) {
    using NResources::EResourceReferenceStatus;
    if (status.Reference == EResourceReferenceStatus::VALID || status.Reference == EResourceReferenceStatus::EMPTY) {
        return;
    }
    const auto error = status.Reference == EResourceReferenceStatus::STALE ? NCommon::EError::INVALID_STATE
                                                                           : NCommon::EError::INVALID_ARGUMENT;
    GRAPHICS_ENGINE_THROW(error,
                          "Render object {}:{} has invalid {} resource reference (status {}, slot {}, generation {})",
                          object.Index,
                          object.Generation,
                          role,
                          static_cast<int>(status.Reference),
                          handle.GetSlotIndex(),
                          handle.GetGeneration());
}

template<typename ObjectContainer, typename DiagnosticContainer>
void AppendObject(RenderObjectId id,
                  const NMath::Transform& transform,
                  CapturedResources& captured,
                  ObjectContainer& objects,
                  DiagnosticContainer& diagnostics) {
    if (captured.Policy != EResourceReadinessPolicy::SKIP_OBJECT) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unsupported resource readiness policy");
    }
    ValidateReference(id, "mesh", captured.MeshHandle, captured.Mesh.Status);
    ValidateReference(id, "material", captured.MaterialHandle, captured.Material.Status);
    if (!captured.Mesh.Lease) {
        diagnostics.push_back({id,
                               ERenderResourceRole::MESH,
                               captured.Policy,
                               captured.MeshHandle.GetSlotIndex(),
                               captured.MeshHandle.GetGeneration(),
                               std::move(captured.Mesh.Status)});
    }
    if (!captured.Material.Lease) {
        diagnostics.push_back({id,
                               ERenderResourceRole::MATERIAL,
                               captured.Policy,
                               captured.MaterialHandle.GetSlotIndex(),
                               captured.MaterialHandle.GetGeneration(),
                               std::move(captured.Material.Status)});
    }
    if (captured.Mesh.Lease && captured.Material.Lease) {
        objects.push_back({id, transform, std::move(*captured.Mesh.Lease), std::move(*captured.Material.Lease)});
    }
}

} // namespace

class RenderWorldBuilder final {
public:
    [[nodiscard]] static const RenderWorld&
    Build(std::pmr::memory_resource& memory,
          RenderFrameIdentity frame,
          const std::pmr::vector<RenderView>& views,
          const std::pmr::vector<RenderObject>& objects,
          const std::pmr::vector<NCommon::ResourceUseRecord>& resourceUseRecords,
          const std::pmr::vector<ResourceReadinessDiagnostic>& diagnostics) {
        void* storage = memory.allocate(sizeof(RenderWorld), alignof(RenderWorld));

        return *::new (storage) RenderWorld{frame,
                                            views.data(),
                                            views.size(),
                                            objects.data(),
                                            objects.size(),
                                            resourceUseRecords.data(),
                                            resourceUseRecords.size(),
                                            diagnostics};
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
    auto& diagnostics = storage.Emplace<std::pmr::vector<ResourceReadinessDiagnostic>>(&memory);

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
                auto captured = CaptureResources(resources, renderable);
                const auto previous = objects.size();
                AppendObject(MakeRenderObjectId(entity), transform.Value, captured, objects, diagnostics);
                if (objects.size() == previous) {
                    return;
                }

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
                                     resourceUseRecords,
                                     diagnostics);
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
        CapturedResources Resources;
    };
    struct Chunk {
        std::vector<RenderView> Views;
        std::vector<RenderObject> Objects;
        std::vector<ResourceReadinessDiagnostic> Diagnostics;
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
                // Latch readiness/versions before scheduling; chunks never resample Resources.
                extraction->Objects.push_back({entity, &transform, CaptureResources(resources, renderable)});
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
                [extraction, index, chunkSize, count](NCommon::TaskContext&) {
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
                                ObjectInput& input = extraction->Objects[item];
                                AppendObject(MakeRenderObjectId(input.Entity),
                                             input.Transform->Value,
                                             input.Resources,
                                             chunk.Objects,
                                             chunk.Diagnostics);
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
                auto& diagnostics = storage.Emplace<std::pmr::vector<ResourceReadinessDiagnostic>>(&memory);
                views.reserve(extraction->Views.size());
                objects.reserve(extraction->Objects.size());
                for (Chunk& chunk: extraction->Chunks) {
                    views.insert(views.end(), chunk.Views.begin(), chunk.Views.end());
                    diagnostics.insert(diagnostics.end(),
                                       std::make_move_iterator(chunk.Diagnostics.begin()),
                                       std::make_move_iterator(chunk.Diagnostics.end()));
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
                                                     records,
                                                     diagnostics));
            },
            dependencies);
}

} // namespace NRenderer::NInternal
