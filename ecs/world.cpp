#include "world.h"

#include <algorithm>

namespace NEcs {

Entity World::CreateEntity() {
    if (!FreeEntityIndices_.empty()) {
        const std::uint32_t index = FreeEntityIndices_.back();
        FreeEntityIndices_.pop_back();
        Entities_[index].Alive = true;

        return {
                .Index = index,
                .Generation = Entities_[index].Generation,
        };
    }

    const std::uint32_t index = static_cast<std::uint32_t>(Entities_.size());
    Entities_.push_back({
            .Generation = 0,
            .Alive = true,
    });

    return {
            .Index = index,
            .Generation = 0,
    };
}

World::DeferredCreateEntityCommand::DeferredCreateEntityCommand(Entity entity)
    : Entity_(entity) {
}

void World::DeferredCreateEntityCommand::Apply(World& world) {
    auto& state = world.Entities_[Entity_.Index];
    if (state.Alive || !state.Reserved || state.Generation != Entity_.Generation) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Deferred ECS entity create is no longer valid");
    }

    state.Alive = true;
    state.Reserved = false;
}

World::DeferredDestroyEntityCommand::DeferredDestroyEntityCommand(Entity entity)
    : Entity_(entity) {
}

void World::DeferredDestroyEntityCommand::Apply(World& world) {
    world.DestroyEntity(Entity_);
}

Entity World::DeferCreateEntity() {
    Entity entity;

    {
        std::scoped_lock lock{DeferredStructuralMutex_};

        if (!FreeEntityIndices_.empty()) {
            const std::uint32_t index = FreeEntityIndices_.back();
            FreeEntityIndices_.pop_back();
            Entities_[index].Reserved = true;
            entity = {
                    .Index = index,
                    .Generation = Entities_[index].Generation,
            };
        } else {
            const std::uint32_t index = static_cast<std::uint32_t>(Entities_.size());
            Entities_.push_back({
                    .Generation = 0,
                    .Alive = false,
                    .Reserved = true,
            });
            entity = {
                    .Index = index,
                    .Generation = 0,
            };
        }

        DeferredStructuralCommands_.push_back(std::make_unique<DeferredCreateEntityCommand>(entity));
    }

    return entity;
}

void World::DeferDestroyEntity(Entity entity) {
    PushDeferredCommand(std::make_unique<DeferredDestroyEntityCommand>(entity));
}

void World::DestroyEntity(Entity entity) {
    if (!IsAlive(entity)) {
        return;
    }

    for (auto& [componentType, storage]: ComponentStorageByType_) {
        static_cast<void>(componentType);
        storage->RemoveEntity(entity.Index);
    }

    auto& state = Entities_[entity.Index];
    state.Alive = false;
    state.Reserved = false;
    ++state.Generation;
    FreeEntityIndices_.push_back(entity.Index);
}

bool World::IsAlive(Entity entity) const noexcept {
    return entity.IsValid() && entity.Index < Entities_.size() && Entities_[entity.Index].Alive &&
           Entities_[entity.Index].Generation == entity.Generation;
}

std::size_t World::GetAliveEntityCount() const noexcept {
    return static_cast<std::size_t>(
            std::ranges::count_if(Entities_, [](const EntityState& state) { return state.Alive; }));
}

void World::RegisterSystem(SystemAccessList access, SystemCallback callback) {
    if (!callback) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ECS system callback must be callable");
    }

    Systems_.push_back({
            .Access = std::move(access),
            .Callback = std::move(callback),
    });
}

const std::vector<SystemAccess>& World::GetSystemAccess(std::size_t index) const {
    ValidateSystemIndex(index);
    return Systems_[index].Access;
}

std::size_t World::GetSystemCount() const noexcept {
    return Systems_.size();
}

void World::RunSystems() {
    for (auto& system: Systems_) {
        system.Callback(*this);
    }

    ApplyDeferredStructuralChanges();
}

void World::ApplyDeferredStructuralChanges() {
    std::vector<std::unique_ptr<IDeferredCommand>> commands;

    {
        std::scoped_lock lock{DeferredStructuralMutex_};
        commands.swap(DeferredStructuralCommands_);
    }

    for (auto& command: commands) {
        command->Apply(*this);
    }
}

void World::ValidateAlive(Entity entity) const {
    if (!IsAlive(entity)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ECS entity is not alive");
    }
}

void World::ValidateSystemIndex(std::size_t index) const {
    if (index >= Systems_.size()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ECS system index is out of range");
    }
}

void World::PushDeferredCommand(std::unique_ptr<IDeferredCommand> command) {
    std::scoped_lock lock{DeferredStructuralMutex_};
    DeferredStructuralCommands_.push_back(std::move(command));
}

} // namespace NEcs
