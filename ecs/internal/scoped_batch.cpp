#include "scoped_batch.h"

#include <limits>

namespace NEcs::NInternal {

ScopedBatchContext::ScopedBatchContext(World& world, std::shared_ptr<ResolvedBatch> batch)
    : m_world(world)
    , m_batch(std::move(batch)) {
}
void ScopedBatchContext::Validate(std::type_index type, bool write) const {
    if (!m_batch->Active.load() || m_batch->Thread != std::this_thread::get_id()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Scoped context is outside its invocation");
    }
    if (!std::ranges::any_of(m_batch->Declarations, [&](const SystemAccess& access) {
            return access.Component == type && (!write || access.Mode == ESystemAccessMode::WRITE);
        })) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Undeclared scoped component access");
    }
}
void ScopedBatchContext::ValidateEntity(EntityReference entity) const {
    if (!m_batch->Active.load() || m_batch->Thread != std::this_thread::get_id() ||
        m_world.GetEntityReference(entity.Identity) != entity || !m_world.IsAlive(entity.Identity)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid deferred scoped entity");
    }
}
void ScopedBatchContext::DeferDestroy(EntityReference entity) {
    ValidateEntity(entity);
    m_world.DeferDestroyEntity(entity.Identity);
}

void ScopedWorldAccess::Register(World& world,
                                 std::vector<ScopedComponentAccess> access,
                                 std::function<void(ScopedBatchContext&)> callback,
                                 SystemSchedule schedule,
                                 std::optional<std::uint32_t> structuralOrder) {
    world.ValidateStructuralWriteAllowed();
    if (!callback) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Empty scoped callback");
    }
    SystemAccessList coarse;
    coarse.reserve(access.size());
    for (const auto& declaration: access) {
        if (!declaration.Resolve || declaration.Access.Component == typeid(void) ||
            (declaration.Access.Mode != ESystemAccessMode::READ &&
             declaration.Access.Mode != ESystemAccessMode::WRITE)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid scoped declaration");
        }
        coarse.push_back(declaration.Access);
    }
    auto system = std::make_shared<ScopedSystem>(std::move(access), std::move(callback));
    world.RegisterSystem(std::move(coarse), [](World&) {}, std::move(schedule), structuralOrder);
    world.Systems_.back().Scoped = std::move(system);
}

std::shared_ptr<ResolvedBatch> ScopedWorldAccess::Resolve(World& world, const ScopedSystem& system) {
    auto batch = std::make_shared<ResolvedBatch>();
    for (const auto& declaration: system.Access) {
        batch->Declarations.push_back(declaration.Access);
        const auto& scope = declaration.Scope;
        std::vector<EntityReference> entities;
        switch (scope.Kind) {
        case EAccessScope::ENTITY:
        case EAccessScope::ENTITY_SET:
            if (scope.Begin != 0 || scope.Count != 0 || scope.Entities.empty() ||
                (scope.Kind == EAccessScope::ENTITY && scope.Entities.size() != 1)) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid entity scope");
            }
            entities = scope.Entities;
            for (const auto entity: entities) {
                if (world.GetEntityReference(entity.Identity) != entity || !world.IsAlive(entity.Identity)) {
                    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Foreign or stale scoped entity");
                }
            }
            break;
        case EAccessScope::CHUNK:
        case EAccessScope::WHOLE_STORAGE:
        case EAccessScope::UNKNOWN:
            if (!scope.Entities.empty() ||
                (scope.Kind == EAccessScope::CHUNK
                         ? scope.Count == 0 || scope.Begin > std::numeric_limits<std::uint64_t>::max() - scope.Count
                         : scope.Begin != 0 || scope.Count != 0)) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid storage/chunk scope");
            }
            for (std::size_t index = 0; index < world.Entities_.size(); ++index) {
                if (world.Entities_[index].Alive && (scope.Kind != EAccessScope::CHUNK ||
                                                     (index >= scope.Begin && index - scope.Begin < scope.Count))) {
                    entities.push_back(world.GetEntityReference(
                            {static_cast<std::uint32_t>(index), world.Entities_[index].Generation}));
                }
            }
            break;
        default:
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unsupported ECS scope");
        }
        for (const auto entity: entities) {
            void* component = declaration.Resolve(world, entity.Identity);
            if (!component) {
                continue;
            }
            const bool write = declaration.Access.Mode == ESystemAccessMode::WRITE;
            auto existing = std::ranges::find_if(batch->Entries, [&](const auto& entry) {
                return entry.Type == declaration.Access.Component && entry.Entity == entity;
            });
            if (existing == batch->Entries.end()) {
                batch->Entries.push_back({declaration.Access.Component, entity, component, write});
            } else {
                existing->Writable = existing->Writable || write;
            }
        }
    }
    return batch;
}

void ScopedWorldAccess::Invoke(World& world, const ScopedSystem& system, const std::shared_ptr<ResolvedBatch>& batch) {
    batch->Thread = std::this_thread::get_id();
    batch->Active.store(true);
    ScopedBatchContext context{world, batch};
    try {
        system.Callback(context);
    } catch (...) {
        batch->Active.store(false);
        throw;
    }
    batch->Active.store(false);
}
ScopedDirtySummary ScopedWorldAccess::TakeDirtySummary(World& world) {
    world.ValidateStructuralWriteAllowed();
    ScopedDirtySummary summary{world.ScopedDirtyAll_, {}};
    summary.Types.swap(world.ScopedDirtyTypes_);
    world.ScopedDirtyAll_ = false;
    return summary;
}

} // namespace NEcs::NInternal
