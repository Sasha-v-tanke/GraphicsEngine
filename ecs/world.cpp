#include "world.h"

#include <algorithm>
#include <iterator>

namespace NEcs {

std::uint32_t World::IDeferredCommand::GetReservedEntityCount() const noexcept {
    return 0;
}

void World::IDeferredCommand::RollbackReservation(World& world) {
    static_cast<void>(world);
}

struct World::DeferredStructuralCommandBuffer::State {
    World* Owner = nullptr;
    std::weak_ptr<bool> OwnerLifetime;
    std::uint32_t ProducerOrder = 0;
    bool Submitted = false;
    std::vector<std::unique_ptr<IDeferredCommand>> Commands;
};

World::World()
    : LifetimeToken_(std::make_shared<bool>(true)) {
}

World::~World() {
    LifetimeToken_.reset();
}

World::DeferredStructuralCommandBuffer::DeferredStructuralCommandBuffer(World& owner, std::uint32_t producerOrder)
    : State_(std::make_unique<State>()) {
    State_->Owner = &owner;
    State_->OwnerLifetime = owner.LifetimeToken_;
    State_->ProducerOrder = producerOrder;
}

World::DeferredStructuralCommandBuffer::DeferredStructuralCommandBuffer(DeferredStructuralCommandBuffer&&) noexcept =
        default;

World::DeferredStructuralCommandBuffer&
World::DeferredStructuralCommandBuffer::operator=(DeferredStructuralCommandBuffer&& other) noexcept {
    if (this != &other) {
        if (State_ != nullptr && State_->Owner != nullptr && !State_->OwnerLifetime.expired() && !State_->Submitted) {
            State_->Owner->RollbackDeferredStructuralCommands(State_->Commands);
        }
        State_ = std::move(other.State_);
    }

    return *this;
}

World::DeferredStructuralCommandBuffer::~DeferredStructuralCommandBuffer() {
    if (State_ != nullptr && State_->Owner != nullptr && !State_->OwnerLifetime.expired() && !State_->Submitted) {
        State_->Owner->RollbackDeferredStructuralCommands(State_->Commands);
    }
}

Entity World::CreateEntity() {
    ValidateStructuralWriteAllowed();

    if (PendingDeferredEntityCreateCount_ > 0) {
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

World::DeferredCreateEntityCommand::DeferredCreateEntityCommand(Entity entity, bool reusedFreeSlot)
    : Entity_(entity)
    , ReusedFreeSlot_(reusedFreeSlot) {
}

void World::DeferredCreateEntityCommand::Apply(World& world) {
    if (ReusedFreeSlot_) {
        auto& state = world.Entities_[Entity_.Index];
        if (state.Alive || state.Reserved || state.Generation != Entity_.Generation) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Deferred ECS entity create is no longer valid");
        }

        state.Alive = true;
        --world.PendingDeferredEntityCreateCount_;
        return;
    }

    while (Entity_.Index >= world.Entities_.size()) {
        const std::uint32_t index = static_cast<std::uint32_t>(world.Entities_.size());
        const std::optional<std::uint32_t> generation = world.FindPendingDeferredAppendGeneration(index);
        if (!generation.has_value() && index == Entity_.Index) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Deferred ECS entity create is no longer valid");
        }
        world.Entities_.push_back({
                .Generation = generation.value_or(world.NextNewEntityGeneration_++),
                .Alive = false,
                .Reserved = generation.has_value(),
        });
        if (!generation.has_value()) {
            world.FreeEntityIndices_.push_back(index);
        }
    }

    auto& state = world.Entities_[Entity_.Index];
    if (state.Alive || !state.Reserved || state.Generation != Entity_.Generation) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Deferred ECS entity create is no longer valid");
    }

    state.Alive = true;
    state.Reserved = false;
    --world.PendingDeferredEntityCreateCount_;
    world.RemovePendingDeferredAppendReservation(Entity_.Index);
}

std::uint32_t World::DeferredCreateEntityCommand::GetReservedEntityCount() const noexcept {
    return ReusedFreeSlot_ ? 0 : 1;
}

void World::DeferredCreateEntityCommand::RollbackReservation(World& world) {
    --world.PendingDeferredEntityCreateCount_;

    if (ReusedFreeSlot_) {
        ++world.Entities_[Entity_.Index].Generation;
        world.FreeEntityIndices_.push_back(Entity_.Index);
    } else {
        world.RemovePendingDeferredAppendReservation(Entity_.Index);
    }
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
        entity = ReserveDeferredEntityLocked(IsCollectingSystemDeferredStructuralCommands_
                                                     ? ActiveDeferredStructuralCommands_
                                                     : DeferredStructuralCommands_);
    }

    return entity;
}

Entity World::DeferCreateEntity(DeferredStructuralCommandBuffer& buffer) {
    ValidateDeferredStructuralCommandBuffer(buffer);

    Entity entity;

    {
        std::scoped_lock lock{DeferredStructuralMutex_};
        entity = ReserveDeferredEntityLocked(buffer.State_->Commands);
    }

    return entity;
}

Entity World::ReserveDeferredEntityLocked(std::vector<std::unique_ptr<IDeferredCommand>>& commands) {
    Entity entity;
    bool reusedFreeSlot = false;

    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    if (!FreeEntityIndices_.empty()) {
        index = FreeEntityIndices_.back();
        FreeEntityIndices_.pop_back();
        generation = Entities_[index].Generation;
        reusedFreeSlot = true;
    } else {
        index = FindNextDeferredAppendIndex();
        generation = NextNewEntityGeneration_++;
        PendingDeferredAppendEntities_.push_back({
                .Index = index,
                .Generation = generation,
        });
    }

    if (!reusedFreeSlot) {
        ++ReservedDeferredEntityCount_;
    }
    ++PendingDeferredEntityCreateCount_;
    entity = {
            .Index = index,
            .Generation = generation,
    };

    commands.push_back(std::make_unique<DeferredCreateEntityCommand>(entity, reusedFreeSlot));

    return entity;
}

std::optional<std::uint32_t> World::FindPendingDeferredAppendGeneration(std::uint32_t index) const noexcept {
    const auto iterator = std::ranges::find_if(PendingDeferredAppendEntities_,
                                               [index](Entity entity) { return entity.Index == index; });

    if (iterator == PendingDeferredAppendEntities_.end()) {
        return std::nullopt;
    }

    return iterator->Generation;
}

bool World::HasPendingDeferredAppendReservation(std::uint32_t index) const noexcept {
    return FindPendingDeferredAppendGeneration(index).has_value();
}

void World::RemovePendingDeferredAppendReservation(std::uint32_t index) {
    const auto iterator = std::ranges::find_if(PendingDeferredAppendEntities_,
                                               [index](Entity entity) { return entity.Index == index; });

    if (iterator != PendingDeferredAppendEntities_.end()) {
        PendingDeferredAppendEntities_.erase(iterator);
    }
}

std::uint32_t World::FindNextDeferredAppendIndex() const noexcept {
    std::uint32_t index = static_cast<std::uint32_t>(Entities_.size());

    while (HasPendingDeferredAppendReservation(index)) {
        ++index;
    }

    return index;
}

void World::DeferDestroyEntity(Entity entity) {
    PushDeferredCommand(std::make_unique<DeferredDestroyEntityCommand>(entity));
}

void World::DeferDestroyEntity(DeferredStructuralCommandBuffer& buffer, Entity entity) {
    PushDeferredCommand(buffer, std::make_unique<DeferredDestroyEntityCommand>(entity));
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

World::DeferredStructuralCommandBuffer World::CreateDeferredStructuralCommandBuffer(std::uint32_t producerOrder) {
    return DeferredStructuralCommandBuffer{*this, producerOrder};
}

void World::SubmitDeferredStructuralCommands(DeferredStructuralCommandBuffer&& buffer) {
    ValidateDeferredStructuralCommandBuffer(buffer);

    std::scoped_lock lock{DeferredStructuralMutex_};
    if (std::ranges::any_of(DeferredStructuralBatches_, [&](const DeferredCommandBatch& batch) {
            return batch.ProducerOrder == buffer.State_->ProducerOrder;
        })) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "ECS deferred command buffer producer order must be unique");
    }

    DeferredCommandBatch batch{
            .ProducerOrder = buffer.State_->ProducerOrder,
            .Commands = std::move(buffer.State_->Commands),
    };

    AppendDeferredCommandBatch(std::move(batch));
    buffer.State_->Submitted = true;
}

void World::RunSystems() {
    for (std::size_t systemIndex = 0; systemIndex < Systems_.size(); ++systemIndex) {
        auto& system = Systems_[systemIndex];
        ActiveDeferredStructuralCommands_.clear();
        IsCollectingSystemDeferredStructuralCommands_ = true;
        IsRunningSystems_ = true;

        try {
            system.Callback(*this);
        } catch (...) {
            for (const auto& command: ActiveDeferredStructuralCommands_) {
                ReservedDeferredEntityCount_ -= command->GetReservedEntityCount();
                command->RollbackReservation(*this);
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
            AppendDeferredCommandBatch({
                    .ProducerOrder = static_cast<std::uint32_t>(systemIndex),
                    .Commands = std::move(ActiveDeferredStructuralCommands_),
            });
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
        std::ranges::sort(DeferredStructuralBatches_,
                          [](const DeferredCommandBatch& lhs, const DeferredCommandBatch& rhs) {
                              if (lhs.ProducerOrder != rhs.ProducerOrder) {
                                  return lhs.ProducerOrder < rhs.ProducerOrder;
                              }

                              return lhs.SubmissionOrder < rhs.SubmissionOrder;
                          });

        for (auto& batch: DeferredStructuralBatches_) {
            DeferredStructuralCommands_.insert(DeferredStructuralCommands_.end(),
                                               std::make_move_iterator(batch.Commands.begin()),
                                               std::make_move_iterator(batch.Commands.end()));
        }
        DeferredStructuralBatches_.clear();
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
    PendingDeferredEntityCreateCount_ = 0;
    PendingDeferredAppendEntities_.clear();
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

void World::ValidateDeferredStructuralCommandBuffer(const DeferredStructuralCommandBuffer& buffer) const {
    if (buffer.State_ == nullptr || buffer.State_->Owner != this || buffer.State_->OwnerLifetime.expired() ||
        buffer.State_->Submitted) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "ECS deferred command buffer does not belong to world");
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

void World::PushDeferredCommand(DeferredStructuralCommandBuffer& buffer, std::unique_ptr<IDeferredCommand> command) {
    if (buffer.State_ == nullptr || buffer.State_->Owner == nullptr || buffer.State_->OwnerLifetime.expired() ||
        buffer.State_->Submitted) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "ECS deferred command buffer does not belong to world");
    }

    buffer.State_->Commands.push_back(std::move(command));
}

void World::AppendDeferredCommandBatch(DeferredCommandBatch batch) {
    batch.SubmissionOrder = NextDeferredBatchSubmissionOrder_++;
    DeferredStructuralBatches_.push_back(std::move(batch));
}

void World::RollbackDeferredStructuralCommands(std::vector<std::unique_ptr<IDeferredCommand>>& commands) {
    std::scoped_lock lock{DeferredStructuralMutex_};

    for (const auto& command: commands) {
        ReservedDeferredEntityCount_ -= command->GetReservedEntityCount();
        command->RollbackReservation(*this);
    }

    commands.clear();
}

} // namespace NEcs
