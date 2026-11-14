#include "extraction.h"
#include "render_components.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <GraphicsEngine/graphics/material.h>
#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>
#include <GraphicsEngine/resources/mesh_data.h>
#include <ecs/internal/transform_resolver.h>
#include <ecs/internal/world_changes.h>

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
    template<typename T>
    static NResources::ResourceLease<T> Retain(const NResources::ResourceLease<T>& lease) {
        return {lease.m_resource, lease.m_version, lease.m_payload, lease.m_gpu};
    }

    template<typename Views, typename Objects, typename Records, typename Diagnostics>
    [[nodiscard]] static const RenderWorld& Build(std::pmr::memory_resource& memory,
                                                  RenderFrameIdentity frame,
                                                  const Views& views,
                                                  const Objects& objects,
                                                  const Records& resourceUseRecords,
                                                  const Diagnostics& diagnostics,
                                                  RenderWorldDelta delta = {}) {
        void* storage = memory.allocate(sizeof(RenderWorld), alignof(RenderWorld));

        return *::new (storage) RenderWorld{frame,
                                            views.data(),
                                            views.size(),
                                            objects.data(),
                                            objects.size(),
                                            resourceUseRecords.data(),
                                            resourceUseRecords.size(),
                                            diagnostics,
                                            delta};
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

namespace {

struct ExtractionInput {
    NEcs::Entity Entity;
    TransformComponent Transform;
    CameraComponent Camera;
    RenderableComponent Renderable;
};

struct ExtractionInputs {
    std::vector<ExtractionInput> Views;
    std::vector<ExtractionInput> Objects;
};

struct ResourceSelection {
    std::uint64_t MeshVersion = 0;
    std::uint64_t MaterialVersion = 0;
    NResources::ResourceStatus Mesh;
    NResources::ResourceStatus Material;
};

struct ExtractionData {
    std::vector<RenderView> Views;
    std::vector<RenderObject> Objects;
    std::vector<NCommon::ResourceUseRecord> Records;
    std::vector<ResourceReadinessDiagnostic> Diagnostics;
    std::map<std::uint32_t, NMath::Mat4> Matrices;
    std::vector<ResourceSelection> Selections;
};

const RenderWorld& PublishExtraction(const NEngine::NController::FrameStorage& storage,
                                     std::shared_ptr<const ExtractionData> data,
                                     RenderWorldDelta delta) {
    (void)storage.Emplace<std::shared_ptr<const ExtractionData>>(data);
    const auto frame = storage.GetFrame();
    return RenderWorldBuilder::Build(storage.GetMemoryResource(),
                                     {frame.GetApplicationFrameIndex(),
                                      frame.GetSimulationIndex(),
                                      frame.GetFrameSlotIndex(),
                                      frame.GetGeneration()},
                                     data->Views,
                                     data->Objects,
                                     data->Records,
                                     data->Diagnostics,
                                     delta);
}

bool Contains(const NCommon::ChangeCapture& changes, NEcs::Entity entity) {
    return changes.FullDirty || std::ranges::any_of(changes.Ranges, [entity](const NCommon::DirtyRange range) {
               return entity.Index >= range.Begin && entity.Index - range.Begin < range.Count;
           });
}

bool SameMatrix(const NMath::Mat4& left, const NMath::Mat4& right) {
    return left.GetValues() == right.GetValues();
}

bool SameStatus(const NResources::ResourceStatus& left, const NResources::ResourceStatus& right) {
    if (left.Reference != right.Reference || left.Identity != right.Identity || left.State != right.State ||
        left.Failure.has_value() != right.Failure.has_value()) {
        return false;
    }
    return !left.Failure ||
           (left.Failure->Code == right.Failure->Code && left.Failure->Message == right.Failure->Message);
}

bool SameDiagnostic(const ResourceReadinessDiagnostic& left, const ResourceReadinessDiagnostic& right) {
    return left.Object == right.Object && left.Role == right.Role && left.Policy == right.Policy &&
           left.ResourceSlot == right.ResourceSlot && left.ResourceGeneration == right.ResourceGeneration &&
           SameStatus(left.Source, right.Source);
}

template<typename T>
class ObservationCache {
public:
    const NResources::ResourceObservation<T>& Observe(const NResources::ResourceManager& resources,
                                                      NResources::ResourceHandle<T> handle,
                                                      std::size_t& observations) {
        const auto key = std::pair{handle.GetSlotIndex(), handle.GetGeneration()};
        auto found = m_values.find(key);
        if (found == m_values.end() || found->second.first != handle) {
            ++observations;
            auto observation = resources.Observe(handle);
            // Never alias foreign owner IDs sharing slot/generation with a valid handle.
            ValidateReference({}, "captured", handle, observation.Status);
            found = m_values.insert_or_assign(key, std::pair{handle, std::move(observation)}).first;
        }
        return found->second.second;
    }

private:
    std::map<std::pair<std::size_t, std::uint64_t>,
             std::pair<NResources::ResourceHandle<T>, NResources::ResourceObservation<T>>>
            m_values;
};

} // namespace

struct IncrementalExtraction::State {
    mutable std::mutex Mutex;
    bool Busy = false;
    std::uint64_t WorldId = 0;
    std::uint64_t TopologyEpoch = 0;
    NCommon::ChangeCursor Cursor;
    std::optional<RenderFrameIdentity> LastFrame;
    std::shared_ptr<const ExtractionInputs> Inputs;
    std::shared_ptr<const ExtractionData> Data;
    std::shared_ptr<NEcs::NInternal::TransformResolver> Resolver;
    ExtractionStatistics Statistics;

    struct Admission {
        std::shared_ptr<State> Owner;
        ~Admission() {
            if (Owner) {
                const std::lock_guard lock{Owner->Mutex};
                Owner->Busy = false;
            }
        }
    };
};

IncrementalExtraction::IncrementalExtraction()
    : m_state(std::make_shared<State>()) {
}
IncrementalExtraction::~IncrementalExtraction() = default;

ExtractionStatistics IncrementalExtraction::GetStatistics() const {
    const std::lock_guard lock{m_state->Mutex};
    return m_state->Statistics;
}

void IncrementalExtraction::Reset() {
    std::shared_ptr<const ExtractionInputs> inputs;
    std::shared_ptr<const ExtractionData> data;
    std::shared_ptr<NEcs::NInternal::TransformResolver> resolver;
    {
        const std::lock_guard lock{m_state->Mutex};
        if (m_state->Busy) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Extraction is pending");
        }
        inputs.swap(m_state->Inputs);
        data.swap(m_state->Data);
        resolver.swap(m_state->Resolver);
        m_state->WorldId = 0;
        m_state->Cursor = {};
        m_state->LastFrame.reset();
        m_state->Statistics = {};
    }
    // Resource payload destructors are user code and may reenter observation.
}

NCommon::TaskHandle IncrementalExtraction::Schedule(NEcs::World& world,
                                                    const NResources::ResourceManager& resources,
                                                    const NEngine::NController::FrameStorage& storage,
                                                    NCommon::TaskSystem& tasks,
                                                    NCommon::TaskContext& task,
                                                    std::function<void(const RenderWorld&)> completion) {
    if (!task.IsCurrentInvocation(tasks) || !completion ||
        storage.GetState() != NEngine::NController::EFrameState::FINALIZE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Extraction requires its active task and finalize frame");
    }
    auto admission = std::make_shared<State::Admission>();
    {
        const std::lock_guard lock{m_state->Mutex};
        if (m_state->Busy) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Extraction is already pending");
        }
        m_state->Busy = true;
        admission->Owner = m_state;
    }
    // The owner-phase barrier authorizes publication; the metadata snapshot is not a lock on World.
    using NEcs::NInternal::WorldChangeAccess;
    const auto transformDomain = WorldChangeAccess::Domain<TransformComponent>(world);
    const auto cameraDomain = WorldChangeAccess::Domain<CameraComponent>(world);
    const auto objectDomain = WorldChangeAccess::Domain<RenderableComponent>(world);
    (void)WorldChangeAccess::PublishEpoch(world);
    const auto changes = WorldChangeAccess::CaptureSnapshot(world);
    const auto since =
            m_state->Cursor.TrackerId == changes.GetCursor().TrackerId ? m_state->Cursor : NCommon::ChangeCursor{};
    const auto transformChanges = changes.CaptureChanges(since, transformDomain);
    const auto cameraChanges = changes.CaptureChanges(since, cameraDomain);
    const auto objectChanges = changes.CaptureChanges(since, objectDomain);
    auto topology = world.AcquireTopologySnapshot();
    const std::uint64_t worldId = topology.GetWorldId();
    const std::uint64_t topologyEpoch = topology.GetEpoch();
    const bool full = !m_state->Inputs || m_state->WorldId != worldId || m_state->TopologyEpoch != topologyEpoch ||
                      transformChanges.FullDirty || cameraChanges.FullDirty || objectChanges.FullDirty;
    const bool dirty =
            full || transformChanges.HasChanges() || cameraChanges.HasChanges() || objectChanges.HasChanges();
    std::shared_ptr<const ExtractionInputs> inputs = m_state->Inputs;
    if (dirty) {
        auto captured = std::make_shared<ExtractionInputs>();
        if (full) {
            std::as_const(world).Query<TransformComponent, CameraComponent>(
                    [&](NEcs::Entity entity, const TransformComponent& transform, const CameraComponent& camera) {
                        captured->Views.push_back(
                                {.Entity = entity, .Transform = transform, .Camera = camera, .Renderable = {}});
                    });
            std::as_const(world).Query<TransformComponent, RenderableComponent>(
                    [&](NEcs::Entity entity,
                        const TransformComponent& transform,
                        const RenderableComponent& renderable) {
                        captured->Objects.push_back(
                                {.Entity = entity, .Transform = transform, .Camera = {}, .Renderable = renderable});
                    });
        } else {
            *captured = *inputs;
            const auto& source = std::as_const(world);
            for (auto& input: captured->Views) {
                if (Contains(transformChanges, input.Entity)) {
                    input.Transform = source.GetComponent<TransformComponent>(input.Entity);
                }
                if (Contains(cameraChanges, input.Entity)) {
                    input.Camera = source.GetComponent<CameraComponent>(input.Entity);
                }
            }
            for (auto& input: captured->Objects) {
                if (Contains(transformChanges, input.Entity)) {
                    input.Transform = source.GetComponent<TransformComponent>(input.Entity);
                }
                if (Contains(objectChanges, input.Entity)) {
                    input.Renderable = source.GetComponent<RenderableComponent>(input.Entity);
                }
            }
        }
        inputs = std::move(captured);
    }
    ExtractionStatistics statistics{.FullRebuild = full};
    std::vector<CapturedResources> capturedResources;
    capturedResources.reserve(inputs->Objects.size());
    ObservationCache<NResources::MeshData> meshes;
    ObservationCache<NGraphics::Material> materials;
    for (const auto& input: inputs->Objects) {
        const auto& mesh = meshes.Observe(resources, input.Renderable.Mesh, statistics.ResourceObservations);
        const auto& material = materials.Observe(resources, input.Renderable.Material, statistics.ResourceObservations);
        CapturedResources captured{.MeshHandle = input.Renderable.Mesh,
                                   .MaterialHandle = input.Renderable.Material,
                                   .Policy = input.Renderable.ReadinessPolicy,
                                   .Mesh = {.Status = mesh.Status, .Lease = {}},
                                   .Material = {.Status = material.Status, .Lease = {}}};
        if (mesh.Lease) {
            captured.Mesh.Lease = RenderWorldBuilder::Retain(*mesh.Lease);
        }
        if (material.Lease) {
            captured.Material.Lease = RenderWorldBuilder::Retain(*material.Lease);
        }
        capturedResources.push_back(std::move(captured));
    }
    auto resolver = m_state->Resolver;
    std::optional<NEcs::NInternal::TransformResolve> resolve;
    std::vector<NCommon::TaskHandle> dependencies{task.GetTask()};
    if (dirty) {
        if (!resolver || m_state->WorldId != worldId) {
            resolver = std::make_shared<NEcs::NInternal::TransformResolver>();
        }
        std::vector<NEcs::EntityReference> required;
        required.reserve(inputs->Views.size() + inputs->Objects.size());
        for (const auto& view: inputs->Views) {
            required.push_back({worldId, view.Entity});
        }
        for (const auto& object: inputs->Objects) {
            required.push_back({worldId, object.Entity});
        }
        resolve = resolver->ScheduleResolve(topology, required, tasks);
        dependencies.push_back(resolve->GetCompletion());
    }
    const auto state = m_state;
    const auto previous = state->WorldId == worldId ? state->Data : nullptr;
    const auto previousFrame = previous ? state->LastFrame : std::nullopt;
    // Copies, not pointers into World, cross the capture boundary. Resource observations were latched above.
    return task.Spawn(
            [state,
             admission,
             inputs,
             previous,
             previousFrame,
             resolver,
             resolve,
             resources = std::make_shared<std::vector<CapturedResources>>(std::move(capturedResources)),
             storage,
             completion = std::move(completion),
             statistics,
             dirty,
             cursor = changes.GetCursor(),
             worldId,
             topologyEpoch,
             &tasks](NCommon::TaskContext&) mutable {
                if (!dirty && previous && resources->size() == previous->Selections.size()) {
                    bool unchanged = true;
                    for (std::size_t index = 0; index < resources->size(); ++index) {
                        const auto& current = (*resources)[index];
                        const auto& old = previous->Selections[index];
                        if ((current.Mesh.Lease ? current.Mesh.Lease->GetVersion() : 0) != old.MeshVersion ||
                            (current.Material.Lease ? current.Material.Lease->GetVersion() : 0) !=
                                    old.MaterialVersion ||
                            !SameStatus(current.Mesh.Status, old.Mesh) ||
                            !SameStatus(current.Material.Status, old.Material)) {
                            unchanged = false;
                            break;
                        }
                    }
                    if (unchanged) {
                        completion(PublishExtraction(storage,
                                                     previous,
                                                     {.FullRebuild = false,
                                                      .WorldId = worldId,
                                                      .PreviousFrame = previousFrame,
                                                      .UpdatedObjects = {},
                                                      .RemovedObjects = {},
                                                      .UpdatedViews = {},
                                                      .RemovedViews = {}}));
                        const auto frame = storage.GetFrame();
                        const std::lock_guard lock{state->Mutex};
                        state->LastFrame = RenderFrameIdentity{frame.GetApplicationFrameIndex(),
                                                               frame.GetSimulationIndex(),
                                                               frame.GetFrameSlotIndex(),
                                                               frame.GetGeneration()};
                        state->Cursor = cursor;
                        state->Statistics = statistics;
                        return;
                    }
                }
                auto data = std::make_shared<ExtractionData>();
                std::optional<NEcs::NInternal::ResolvedTransforms> transforms;
                if (resolve) {
                    transforms = resolve->ReadResolved(tasks);
                    statistics.RecomputedTransforms = transforms->GetRecomputedCount();
                }
                std::vector<RenderObjectId> updated;
                std::vector<RenderObjectId> removed;
                std::vector<RenderObjectId> updatedViews;
                std::vector<RenderObjectId> removedViews;
                data->Views.reserve(inputs->Views.size());
                data->Objects.reserve(inputs->Objects.size());
                std::map<std::uint32_t, const RenderView*> oldViews;
                std::map<std::uint32_t, const RenderObject*> oldObjects;
                if (previous) {
                    for (const auto& view: previous->Views) {
                        oldViews.emplace(view.Id.Index, &view);
                    }
                    for (const auto& object: previous->Objects) {
                        oldObjects.emplace(object.Id.Index, &object);
                    }
                }
                for (const auto& input: inputs->Views) {
                    const auto id = MakeRenderObjectId(input.Entity);
                    const auto found = oldViews.find(id.Index);
                    const RenderView* old = found == oldViews.end() ? nullptr : found->second;
                    NMath::Mat4 matrix = transforms ? transforms->Get({worldId, input.Entity}).Value *
                                                              NMath::ComposeTransform(input.Transform.Value)
                                                    : *old->ResolvedWorldTransform;
                    const auto& projection = input.Camera.Projection;
                    const bool same = old && old->Id == id && SameMatrix(*old->ResolvedWorldTransform, matrix) &&
                                      old->Projection.VerticalFovRadians == projection.VerticalFovRadians &&
                                      old->Projection.AspectRatio == projection.AspectRatio &&
                                      old->Projection.NearPlane == projection.NearPlane &&
                                      old->Projection.FarPlane == projection.FarPlane;
                    if (!same) {
                        updatedViews.push_back(id);
                    }
                    data->Views.push_back({id, input.Transform.Value, projection, matrix});
                    if (old && old->Id != id) {
                        removedViews.push_back(old->Id);
                    }
                    oldViews.erase(id.Index);
                }
                for (const auto& [index, old]: oldViews) {
                    (void)index;
                    removedViews.push_back(old->Id);
                }
                for (std::size_t index = 0; index < inputs->Objects.size(); ++index) {
                    const auto& input = inputs->Objects[index];
                    const auto id = MakeRenderObjectId(input.Entity);
                    const auto found = oldObjects.find(id.Index);
                    const RenderObject* old = found == oldObjects.end() ? nullptr : found->second;
                    NMath::Mat4 matrix = transforms ? transforms->Get({worldId, input.Entity}).Value *
                                                              NMath::ComposeTransform(input.Transform.Value)
                                       : old        ? *old->ResolvedWorldTransform
                                                    : NMath::Mat4::Identity();
                    // A skipped object also needs its matrix for a future readiness transition.
                    if (!transforms && !old) {
                        // Cached input already owns the last resolved matrix (see publication below).
                        matrix = previous->Matrices.at(input.Entity.Index);
                    }
                    data->Matrices.emplace(input.Entity.Index, matrix);
                    auto& captured = (*resources)[index];
                    data->Selections.push_back({captured.Mesh.Lease ? captured.Mesh.Lease->GetVersion() : 0,
                                                captured.Material.Lease ? captured.Material.Lease->GetVersion() : 0,
                                                captured.Mesh.Status,
                                                captured.Material.Status});
                    const bool same = old && old->Id == id && captured.Mesh.Lease && captured.Material.Lease &&
                                      captured.MeshHandle == old->Mesh.GetResource() &&
                                      captured.MaterialHandle == old->Material.GetResource() &&
                                      captured.Mesh.Lease->GetVersion() == old->Mesh.GetVersion() &&
                                      captured.Material.Lease->GetVersion() == old->Material.GetVersion() &&
                                      SameMatrix(*old->ResolvedWorldTransform, matrix);
                    const std::size_t before = data->Objects.size();
                    AppendObject(id, input.Transform.Value, captured, data->Objects, data->Diagnostics);
                    if (before != data->Objects.size()) {
                        auto& object = data->Objects.back();
                        object.ResolvedWorldTransform = matrix;
                        data->Records.push_back(object.Mesh.GetUseRecord());
                        data->Records.push_back(object.Material.GetUseRecord());
                        if (!same) {
                            updated.push_back(id);
                        }
                    }
                    if (old && (old->Id != id || before == data->Objects.size())) {
                        removed.push_back(old->Id);
                    }
                    oldObjects.erase(id.Index);
                }
                for (const auto& [index, old]: oldObjects) {
                    (void)index;
                    removed.push_back(old->Id);
                }
                const bool sameDiagnostics = previous && data->Diagnostics.size() == previous->Diagnostics.size() &&
                                             std::equal(data->Diagnostics.begin(),
                                                        data->Diagnostics.end(),
                                                        previous->Diagnostics.begin(),
                                                        SameDiagnostic);
                statistics.UpdatedObjects = updated.size();
                statistics.UpdatedViews = updatedViews.size();
                std::shared_ptr<const ExtractionData> published = data;
                if (previous && updated.empty() && removed.empty() && updatedViews.empty() && removedViews.empty() &&
                    data->Views.size() == previous->Views.size() && sameDiagnostics) {
                    published = previous;
                }

                auto& delta = storage.Emplace<RenderWorldDelta>();
                delta.FullRebuild = statistics.FullRebuild;
                delta.WorldId = worldId;
                delta.PreviousFrame = previousFrame;
                auto& updates = storage.Emplace<std::vector<RenderObjectId>>(std::move(updated));
                auto& removals = storage.Emplace<std::vector<RenderObjectId>>(std::move(removed));
                auto& viewUpdates = storage.Emplace<std::vector<RenderObjectId>>(std::move(updatedViews));
                delta.UpdatedObjects = updates;
                delta.RemovedObjects = removals;
                delta.UpdatedViews = viewUpdates;
                auto& viewRemovals = storage.Emplace<std::vector<RenderObjectId>>(std::move(removedViews));
                delta.RemovedViews = viewRemovals;
                const auto& snapshot = PublishExtraction(storage, published, delta);
                completion(snapshot);
                // Successful publication only; failure/cancellation keeps the previous cursor and immutable cache.
                std::shared_ptr<const ExtractionData> retired;
                {
                    const std::lock_guard lock{state->Mutex};
                    retired = std::exchange(state->Data, std::move(published));
                    state->Inputs = inputs;
                    state->Resolver = resolver;
                    const auto frame = storage.GetFrame();
                    state->LastFrame = RenderFrameIdentity{frame.GetApplicationFrameIndex(),
                                                           frame.GetSimulationIndex(),
                                                           frame.GetFrameSlotIndex(),
                                                           frame.GetGeneration()};
                    state->Cursor = cursor;
                    state->WorldId = worldId;
                    state->TopologyEpoch = topologyEpoch;
                    state->Statistics = statistics;
                }
            },
            dependencies);
}

} // namespace NRenderer::NInternal
