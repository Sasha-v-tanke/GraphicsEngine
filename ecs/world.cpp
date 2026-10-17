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
    std::vector<DeferredEntityReservation> EntityReservations;
    std::size_t NextEntityReservation = 0;
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
            std::scoped_lock lock{State_->Owner->DeferredStructuralMutex_};
            State_->Owner->ReleaseUnusedDeferredEntityReservationsLocked(State_->EntityReservations,
                                                                         State_->NextEntityReservation);
        }
        State_ = std::move(other.State_);
    }

    return *this;
}

World::DeferredStructuralCommandBuffer::~DeferredStructuralCommandBuffer() {
    if (State_ != nullptr && State_->Owner != nullptr && !State_->OwnerLifetime.expired() && !State_->Submitted) {
        State_->Owner->RollbackDeferredStructuralCommands(State_->Commands);
        std::scoped_lock lock{State_->Owner->DeferredStructuralMutex_};
        State_->Owner->ReleaseUnusedDeferredEntityReservationsLocked(State_->EntityReservations,
                                                                     State_->NextEntityReservation);
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
        if (IsCollectingSystemDeferredStructuralCommands_) {
            ValidateSystemDeferredCommandCollectionLocked();
        }
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
        if (buffer.State_->NextEntityReservation == buffer.State_->EntityReservations.size()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "ECS producer did not declare enough deferred entity reservations");
        }
        const DeferredEntityReservation& reservation =
                buffer.State_->EntityReservations[buffer.State_->NextEntityReservation++];
        buffer.State_->Commands.push_back(
                std::make_unique<DeferredCreateEntityCommand>(reservation.Identity, reservation.ReusesFreeSlot));
        entity = reservation.Identity;
    }

    return entity;
}

Entity World::ReserveDeferredEntityLocked(std::vector<std::unique_ptr<IDeferredCommand>>& commands) {
    const DeferredEntityReservation reservation = ReserveDeferredEntityLocked();
    commands.push_back(std::make_unique<DeferredCreateEntityCommand>(reservation.Identity, reservation.ReusesFreeSlot));
    return reservation.Identity;
}

World::DeferredEntityReservation World::ReserveDeferredEntityLocked() {
    DeferredEntityReservation reservation;

    if (!FreeEntityIndices_.empty()) {
        const std::uint32_t index = FreeEntityIndices_.back();
        FreeEntityIndices_.pop_back();
        reservation = {.Identity = {.Index = index, .Generation = Entities_[index].Generation}, .ReusesFreeSlot = true};
    } else {
        reservation = {.Identity = {.Index = FindNextDeferredAppendIndex(), .Generation = NextNewEntityGeneration_++}};
        PendingDeferredAppendEntities_.push_back(reservation.Identity);
    }

    if (!reservation.ReusesFreeSlot) {
        ++ReservedDeferredEntityCount_;
    }
    ++PendingDeferredEntityCreateCount_;
    return reservation;
}

void World::ReleaseDeferredEntityReservationLocked(const DeferredEntityReservation& reservation) {
    --PendingDeferredEntityCreateCount_;
    if (reservation.ReusesFreeSlot) {
        ++Entities_[reservation.Identity.Index].Generation;
        FreeEntityIndices_.push_back(reservation.Identity.Index);
        return;
    }
    --ReservedDeferredEntityCount_;
    RemovePendingDeferredAppendReservation(reservation.Identity.Index);
}

void World::ReleaseUnusedDeferredEntityReservationsLocked(std::vector<DeferredEntityReservation>& reservations,
                                                          std::size_t firstUnused) {
    for (std::size_t index = firstUnused; index < reservations.size(); ++index) {
        ReleaseDeferredEntityReservationLocked(reservations[index]);
    }
    reservations.resize(firstUnused);
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
            .StructuralOrder = std::nullopt,
    });
}

void World::RegisterSystem(SystemAccessList access, SystemCallback callback, std::uint32_t structuralOrder) {
    if (!callback) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ECS system callback must be callable");
    }
    if (std::ranges::any_of(Systems_, [structuralOrder](const RegisteredSystem& system) {
            return system.StructuralOrder == structuralOrder;
        })) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ECS system structural orders must be unique");
    }

    Systems_.push_back({
            .Access = std::move(access),
            .Callback = std::move(callback),
            .StructuralOrder = structuralOrder,
    });
}

const std::vector<SystemAccess>& World::GetSystemAccess(std::size_t index) const {
    ValidateSystemIndex(index);
    return Systems_[index].Access;
}

std::size_t World::GetSystemCount() const noexcept {
    return Systems_.size();
}

void World::BeginDeferredStructuralPhase(std::vector<DeferredProducer> producers) {
    std::ranges::sort(producers, {}, &DeferredProducer::Order);

    std::scoped_lock lock{DeferredStructuralMutex_};
    if (PendingDeferredEntityCreateCount_ != 0 || !PlannedDeferredEntityReservations_.empty()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "ECS deferred structural phase is already active");
    }

    for (std::size_t index = 0; index < producers.size(); ++index) {
        if (index > 0 && producers[index - 1].Order == producers[index].Order) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ECS deferred producer orders must be unique");
        }
        auto& reservations = PlannedDeferredEntityReservations_[producers[index].Order];
        reservations.reserve(producers[index].EntityCreateCount);
        for (std::uint32_t count = 0; count < producers[index].EntityCreateCount; ++count) {
            reservations.push_back(ReserveDeferredEntityLocked());
        }
    }
}

World::DeferredStructuralCommandBuffer World::CreateDeferredStructuralCommandBuffer(std::uint32_t producerOrder) {
    DeferredStructuralCommandBuffer buffer{*this, producerOrder};
    std::scoped_lock lock{DeferredStructuralMutex_};
    if (const auto iterator = PlannedDeferredEntityReservations_.find(producerOrder);
        iterator != PlannedDeferredEntityReservations_.end()) {
        buffer.State_->EntityReservations = std::move(iterator->second);
        PlannedDeferredEntityReservations_.erase(iterator);
    }
    return buffer;
}

void World::SubmitDeferredStructuralCommands(DeferredStructuralCommandBuffer&& buffer) {
    ValidateDeferredStructuralCommandBuffer(buffer);

    std::scoped_lock lock{DeferredStructuralMutex_};
    if (std::ranges::any_of(DeferredStructuralBatches_, [&](const DeferredCommandBatch& batch) {
            return batch.Source == DeferredCommandBatch::ESource::EXTERNAL &&
                   batch.ProducerOrder == buffer.State_->ProducerOrder;
        })) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "ECS deferred command buffer producer order must be unique");
    }

    DeferredCommandBatch batch{
            .Source = DeferredCommandBatch::ESource::EXTERNAL,
            .ProducerOrder = buffer.State_->ProducerOrder,
            .Commands = std::move(buffer.State_->Commands),
    };

    ReleaseUnusedDeferredEntityReservationsLocked(buffer.State_->EntityReservations,
                                                  buffer.State_->NextEntityReservation);
    AppendDeferredCommandBatch(std::move(batch));
    buffer.State_->Submitted = true;
}

void World::RunSystems() {
    for (auto& system: Systems_) {
        ActiveDeferredStructuralCommands_.clear();
        IsCollectingSystemDeferredStructuralCommands_ = true;
        IsRunningSystems_ = true;
        ActiveSystemThreadId_ = std::this_thread::get_id();
        ActiveSystemStructuralOrder_ = system.StructuralOrder;

        try {
            system.Callback(*this);
        } catch (...) {
            for (const auto& command: ActiveDeferredStructuralCommands_) {
                ReservedDeferredEntityCount_ -= command->GetReservedEntityCount();
                command->RollbackReservation(*this);
            }
            IsRunningSystems_ = false;
            IsCollectingSystemDeferredStructuralCommands_ = false;
            ActiveSystemThreadId_.reset();
            ActiveSystemStructuralOrder_.reset();
            ActiveDeferredStructuralCommands_.clear();
            throw;
        }

        IsRunningSystems_ = false;
        IsCollectingSystemDeferredStructuralCommands_ = false;
        ActiveSystemThreadId_.reset();

        {
            std::scoped_lock lock{DeferredStructuralMutex_};
            if (!ActiveDeferredStructuralCommands_.empty()) {
                if (!ActiveSystemStructuralOrder_.has_value()) {
                    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                          "ECS system structural commands require an explicit structural order");
                }
                AppendDeferredCommandBatch({
                        .Source = DeferredCommandBatch::ESource::SYSTEM,
                        .ProducerOrder = *ActiveSystemStructuralOrder_,
                        .Commands = std::move(ActiveDeferredStructuralCommands_),
                });
            }
        }
        ActiveSystemStructuralOrder_.reset();
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
        for (auto& [producerOrder, reservations]: PlannedDeferredEntityReservations_) {
            static_cast<void>(producerOrder);
            ReleaseUnusedDeferredEntityReservationsLocked(reservations, 0);
        }
        PlannedDeferredEntityReservations_.clear();
        std::ranges::sort(DeferredStructuralBatches_,
                          [](const DeferredCommandBatch& lhs, const DeferredCommandBatch& rhs) {
                              if (lhs.Source != rhs.Source) {
                                  return lhs.Source < rhs.Source;
                              }
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
    std::scoped_lock lock{DeferredStructuralMutex_};
    if (IsCollectingSystemDeferredStructuralCommands_) {
        ValidateSystemDeferredCommandCollectionLocked();
        ActiveDeferredStructuralCommands_.push_back(std::move(command));
        return;
    }
    DeferredStructuralCommands_.push_back(std::move(command));
}

void World::ValidateSystemDeferredCommandCollectionLocked() const {
    if (!ActiveSystemThreadId_.has_value() || *ActiveSystemThreadId_ != std::this_thread::get_id()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "ECS parallel workers must use producer-local deferred command buffers");
    }
    if (!ActiveSystemStructuralOrder_.has_value()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "ECS system structural commands require an explicit structural order");
    }
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
