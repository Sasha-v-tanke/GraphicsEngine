#include "world.h"

#include <algorithm>
#include <iterator>

namespace NEcs {

std::uint32_t World::IDeferredCommand::GetReservedEntityCount() const noexcept {
    return 0;
}

Entity World::CreateEntity() {
    ValidateStructuralWriteAllowed();

    if (ReservedDeferredEntityCount_ > 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "ECS deferred entity creates must be committed before immediate entity creation");
    }

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
    const std::uint32_t generation = NextNewEntityGeneration_++;
    Entities_.push_back({
            .Generation = generation,
            .Alive = true,
    });

    return {
            .Index = index,
            .Generation = generation,
    };
}

World::DeferredCreateEntityCommand::DeferredCreateEntityCommand(Entity entity)
    : Entity_(entity) {
}

void World::DeferredCreateEntityCommand::Apply(World& world) {
    if (Entity_.Index > world.Entities_.size()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Deferred ECS entity create order is invalid");
    }

    if (Entity_.Index == world.Entities_.size()) {
        world.Entities_.push_back({
                .Generation = Entity_.Generation,
                .Alive = false,
                .Reserved = true,
        });
    }

    auto& state = world.Entities_[Entity_.Index];
    if (state.Alive || !state.Reserved || state.Generation != Entity_.Generation) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Deferred ECS entity create is no longer valid");
    }

    state.Alive = true;
    state.Reserved = false;
}

std::uint32_t World::DeferredCreateEntityCommand::GetReservedEntityCount() const noexcept {
    return 1;
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

        const std::uint32_t index = static_cast<std::uint32_t>(Entities_.size() + ReservedDeferredEntityCount_);
        const std::uint32_t generation = NextNewEntityGeneration_++;
        ++ReservedDeferredEntityCount_;
        entity = {
                .Index = index,
                .Generation = generation,
        };

        if (IsCollectingSystemDeferredStructuralCommands_) {
            ActiveDeferredStructuralCommands_.push_back(std::make_unique<DeferredCreateEntityCommand>(entity));
        } else {
            DeferredStructuralCommands_.push_back(std::make_unique<DeferredCreateEntityCommand>(entity));
        }
    }

    return entity;
}

void World::DeferDestroyEntity(Entity entity) {
    PushDeferredCommand(std::make_unique<DeferredDestroyEntityCommand>(entity));
}

void World::DestroyEntity(Entity entity) {
    ValidateStructuralWriteAllowed();

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
        ActiveDeferredStructuralCommands_.clear();
        IsCollectingSystemDeferredStructuralCommands_ = true;
        IsRunningSystems_ = true;

        try {
            system.Callback(*this);
        } catch (...) {
            for (const auto& command: ActiveDeferredStructuralCommands_) {
                ReservedDeferredEntityCount_ -= command->GetReservedEntityCount();
            }
            IsRunningSystems_ = false;
            IsCollectingSystemDeferredStructuralCommands_ = false;
            ActiveDeferredStructuralCommands_.clear();
            throw;
        }

        IsRunningSystems_ = false;
        IsCollectingSystemDeferredStructuralCommands_ = false;

        {
            std::scoped_lock lock{DeferredStructuralMutex_};
            DeferredStructuralCommands_.insert(DeferredStructuralCommands_.end(),
                                               std::make_move_iterator(ActiveDeferredStructuralCommands_.begin()),
                                               std::make_move_iterator(ActiveDeferredStructuralCommands_.end()));
        }
        ActiveDeferredStructuralCommands_.clear();
    }

    ApplyDeferredStructuralChanges();
}

void World::ApplyDeferredStructuralChanges() {
    if (IsRunningSystems_ && !IsApplyingDeferredStructuralChanges_) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "ECS deferred structural changes cannot be explicitly committed during system execution");
    }

    std::vector<std::unique_ptr<IDeferredCommand>> commands;

    {
        std::scoped_lock lock{DeferredStructuralMutex_};
        commands.swap(DeferredStructuralCommands_);
    }

    IsApplyingDeferredStructuralChanges_ = true;

    std::size_t nextCommand = 0;

    try {
        for (auto& command: commands) {
            command->Apply(*this);
            ++nextCommand;
        }
    } catch (...) {
        IsApplyingDeferredStructuralChanges_ = false;
        {
            std::scoped_lock lock{DeferredStructuralMutex_};
            DeferredStructuralCommands_.insert(
                    DeferredStructuralCommands_.begin(),
                    std::make_move_iterator(commands.begin() + static_cast<std::ptrdiff_t>(nextCommand + 1)),
                    std::make_move_iterator(commands.end()));
            ReservedDeferredEntityCount_ = 0;
            for (const auto& command: DeferredStructuralCommands_) {
                ReservedDeferredEntityCount_ += command->GetReservedEntityCount();
            }
        }
        throw;
    }

    IsApplyingDeferredStructuralChanges_ = false;
    ReservedDeferredEntityCount_ = 0;
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

void World::ValidateStructuralWriteAllowed() const {
    if (IsRunningSystems_ && !IsApplyingDeferredStructuralChanges_) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "ECS structural changes during system execution must be deferred");
    }
}

void World::PushDeferredCommand(std::unique_ptr<IDeferredCommand> command) {
    if (IsCollectingSystemDeferredStructuralCommands_) {
        std::scoped_lock lock{DeferredStructuralMutex_};
        ActiveDeferredStructuralCommands_.push_back(std::move(command));
        return;
    }

    std::scoped_lock lock{DeferredStructuralMutex_};
    DeferredStructuralCommands_.push_back(std::move(command));
}

} // namespace NEcs
