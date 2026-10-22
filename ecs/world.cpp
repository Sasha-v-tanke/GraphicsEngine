#include "world.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <numeric>

#include <GraphicsEngine/lib/thread/task/task_system.h>

namespace NEcs {

struct World::SystemInvocation {
    World* Owner = nullptr;
    std::optional<std::uint32_t> StructuralOrder;
    std::uint64_t Id = 0;
    std::vector<std::unique_ptr<IDeferredCommand>> Commands;
    std::vector<DeferredCommandBatch> WorkerBatches;
    std::exception_ptr Error;
};

thread_local World::SystemInvocation* World::CurrentSystemInvocation_ = nullptr;
thread_local World* World::CommittingWorld_ = nullptr;

World::SystemInvocation* World::GetCurrentSystemInvocation() const noexcept {
    return CurrentSystemInvocation_ != nullptr && CurrentSystemInvocation_->Owner == this ? CurrentSystemInvocation_
                                                                                          : nullptr;
}

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
    std::optional<std::uint32_t> SystemOrder;
    std::uint64_t SystemInvocationId = 0;
    bool Submitted = false;
    bool HasClaimedReservations = false;
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
            if (State_->HasClaimedReservations) {
                --State_->Owner->OutstandingDeferredReservationBufferCount_;
            }
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
        if (State_->HasClaimedReservations) {
            --State_->Owner->OutstandingDeferredReservationBufferCount_;
        }
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
        if (IsRunningSystems_) {
            ValidateSystemDeferredCommandCollectionLocked();
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "ECS system entity creates require a planned producer-local buffer");
        }
        entity = ReserveDeferredEntityLocked(DeferredStructuralCommands_);
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
    ValidateStructuralWriteAllowed();
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
    ValidateStructuralWriteAllowed();
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

void World::RegisterSystem(SystemAccessList access,
                           SystemCallback callback,
                           SystemSchedule schedule,
                           std::optional<std::uint32_t> structuralOrder) {
    if (std::ranges::any_of(Systems_, [&schedule](const RegisteredSystem& system) {
            return system.Schedule.has_value() && system.Schedule->Id == schedule.Id;
        })) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ECS system scheduling IDs must be unique");
    }
    if (structuralOrder.has_value()) {
        RegisterSystem(std::move(access), std::move(callback), *structuralOrder);
    } else {
        RegisterSystem(std::move(access), std::move(callback));
    }
    Systems_.back().Schedule = std::move(schedule);
}

World::SystemExecutionGraph World::BuildSystemExecutionGraph(bool requireSchedules) const {
    const std::size_t count = Systems_.size();
    SystemExecutionGraph graph;
    graph.Order.resize(count);
    std::iota(graph.Order.begin(), graph.Order.end(), 0);
    graph.Dependencies.resize(count);
    const bool hasSchedules =
            std::ranges::any_of(Systems_, [](const RegisteredSystem& system) { return system.Schedule.has_value(); });
    if (!requireSchedules && !hasSchedules) {
        return graph;
    }

    std::map<std::uint64_t, std::size_t> indices;
    for (std::size_t index = 0; index < count; ++index) {
        if (!Systems_[index].Schedule.has_value()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "ECS scheduled execution requires a stable ID for every system");
        }
        indices.emplace(Systems_[index].Schedule->Id, index);
    }
    std::vector<std::vector<bool>> edges(count, std::vector<bool>(count, false));
    auto addConstraint = [&](std::uint64_t before, std::uint64_t after) {
        if (before == after || !indices.contains(before) || !indices.contains(after)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ECS system ordering constraint is invalid");
        }
        edges[indices.at(before)][indices.at(after)] = true;
    };
    for (const auto& system: Systems_) {
        for (const auto before: system.Schedule->Before) {
            addConstraint(system.Schedule->Id, before);
        }
        for (const auto after: system.Schedule->After) {
            addConstraint(after, system.Schedule->Id);
        }
    }

    std::vector<std::size_t> incoming(count, 0);
    for (const auto& outgoing: edges) {
        for (std::size_t target = 0; target < count; ++target) {
            incoming[target] += outgoing[target] ? 1 : 0;
        }
    }
    std::vector<bool> visited(count, false);
    graph.Order.clear();
    for (std::size_t position = 0; position < count; ++position) {
        std::optional<std::size_t> next;
        for (const auto& [id, index]: indices) {
            static_cast<void>(id);
            if (!visited[index] && incoming[index] == 0) {
                next = index;
                break;
            }
        }
        if (!next.has_value()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ECS system ordering constraints contain a cycle");
        }
        visited[*next] = true;
        graph.Order.push_back(*next);
        for (std::size_t target = 0; target < count; ++target) {
            if (edges[*next][target]) {
                --incoming[target];
            }
        }
    }

    for (std::size_t left = 0; left < count; ++left) {
        const std::size_t before = graph.Order[left];
        for (std::size_t right = left + 1; right < count; ++right) {
            const std::size_t after = graph.Order[right];
            const bool conflicts = std::ranges::any_of(Systems_[before].Access, [&](const SystemAccess& lhs) {
                return std::ranges::any_of(Systems_[after].Access, [&](const SystemAccess& rhs) {
                    return lhs.Component == rhs.Component &&
                           (lhs.Mode == ESystemAccessMode::WRITE || rhs.Mode == ESystemAccessMode::WRITE);
                });
            });
            edges[before][after] = edges[before][after] || conflicts;
        }
    }
    for (const std::size_t after: graph.Order) {
        for (const std::size_t before: graph.Order) {
            if (edges[before][after]) {
                graph.Dependencies[after].push_back(before);
            }
        }
    }
    return graph;
}

const std::vector<SystemAccess>& World::GetSystemAccess(std::size_t index) const {
    ValidateSystemIndex(index);
    return Systems_[index].Access;
}

std::size_t World::GetSystemCount() const noexcept {
    return Systems_.size();
}

void World::BeginDeferredStructuralPhase(std::vector<DeferredProducer> producers) {
    std::ranges::sort(producers, [](const DeferredProducer& lhs, const DeferredProducer& rhs) {
        return std::tuple{!lhs.SystemOrder.has_value(), lhs.SystemOrder.value_or(0), lhs.Order} <
               std::tuple{!rhs.SystemOrder.has_value(), rhs.SystemOrder.value_or(0), rhs.Order};
    });

    for (std::size_t index = 1; index < producers.size(); ++index) {
        if (producers[index - 1].SystemOrder == producers[index].SystemOrder &&
            producers[index - 1].Order == producers[index].Order) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "ECS deferred producer orders must be unique");
        }
    }

    std::scoped_lock lock{DeferredStructuralMutex_};
    if (IsRunningSystems_) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "ECS create reservations must be planned before system execution");
    }
    if (!PlannedDeferredEntityReservations_.empty() || PendingDeferredEntityCreateCount_ != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "ECS deferred structural phase is already active");
    }

    for (const auto& producer: producers) {
        auto& reservations = PlannedDeferredEntityReservations_[producer.SystemOrder][producer.Order];
        reservations.reserve(producer.EntityCreateCount);
        for (std::uint32_t count = 0; count < producer.EntityCreateCount; ++count) {
            reservations.push_back(ReserveDeferredEntityLocked());
        }
    }
}

World::DeferredStructuralCommandBuffer World::CreateDeferredStructuralCommandBuffer(std::uint32_t producerOrder) {
    DeferredStructuralCommandBuffer buffer{*this, producerOrder};
    std::scoped_lock lock{DeferredStructuralMutex_};
    if (IsRunningSystems_) {
        ValidateSystemDeferredCommandCollectionLocked();
        const auto* invocation = GetCurrentSystemInvocation();
        buffer.State_->SystemOrder = invocation->StructuralOrder;
        buffer.State_->SystemInvocationId = invocation->Id;
    }
    if (const auto phase = PlannedDeferredEntityReservations_.find(buffer.State_->SystemOrder);
        phase != PlannedDeferredEntityReservations_.end()) {
        if (const auto iterator = phase->second.find(producerOrder); iterator != phase->second.end()) {
            buffer.State_->EntityReservations = std::move(iterator->second);
            phase->second.erase(iterator);
            if (!buffer.State_->EntityReservations.empty()) {
                buffer.State_->HasClaimedReservations = true;
                ++OutstandingDeferredReservationBufferCount_;
            }
        }
    }
    return buffer;
}

void World::SubmitDeferredStructuralCommands(DeferredStructuralCommandBuffer&& buffer) {
    ValidateDeferredStructuralCommandBuffer(buffer);

    std::scoped_lock lock{DeferredStructuralMutex_};
    auto* invocation = GetCurrentSystemInvocation();
    if (IsRunningSystems_) {
        if (invocation == nullptr || !invocation->StructuralOrder.has_value() ||
            buffer.State_->SystemOrder != invocation->StructuralOrder ||
            buffer.State_->SystemInvocationId != invocation->Id) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "ECS worker buffer must be submitted by its owning structural system");
        }
    } else if (buffer.State_->SystemOrder.has_value()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "ECS worker buffer must be submitted before its system returns");
    }
    const auto& batches = invocation != nullptr ? invocation->WorkerBatches : DeferredStructuralBatches_;
    if (std::ranges::any_of(batches, [&](const DeferredCommandBatch& batch) {
            return batch.Source == (buffer.State_->SystemOrder.has_value() ? DeferredCommandBatch::ESource::SYSTEM
                                                                           : DeferredCommandBatch::ESource::EXTERNAL) &&
                   (!buffer.State_->SystemOrder.has_value() ||
                    (batch.IsWorker && batch.SystemOrder == *buffer.State_->SystemOrder)) &&
                   batch.ProducerOrder == buffer.State_->ProducerOrder;
        })) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "ECS deferred command buffer producer order must be unique");
    }

    DeferredCommandBatch batch{
            .Source = buffer.State_->SystemOrder.has_value() ? DeferredCommandBatch::ESource::SYSTEM
                                                             : DeferredCommandBatch::ESource::EXTERNAL,
            .SystemOrder = buffer.State_->SystemOrder.value_or(0),
            .IsWorker = buffer.State_->SystemOrder.has_value(),
            .ProducerOrder = buffer.State_->ProducerOrder,
            .Commands = std::move(buffer.State_->Commands),
    };

    ReleaseUnusedDeferredEntityReservationsLocked(buffer.State_->EntityReservations,
                                                  buffer.State_->NextEntityReservation);
    if (invocation != nullptr) {
        invocation->WorkerBatches.push_back(std::move(batch));
    } else {
        AppendDeferredCommandBatch(std::move(batch));
    }
    if (buffer.State_->HasClaimedReservations) {
        --OutstandingDeferredReservationBufferCount_;
        buffer.State_->HasClaimedReservations = false;
    }
    buffer.State_->Submitted = true;
}

void World::RunSystems() {
    RunSystemsImpl(nullptr);
}

void World::RunSystems(NCommon::TaskSystem& taskSystem) {
    if (NCommon::TaskSystem::IsCurrentThreadWorker()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "ECS parallel execution must be coordinated by a host thread");
    }
    RunSystemsImpl(&taskSystem);
}

void World::ExecuteSystem(std::size_t index, SystemInvocation& invocation) {
    auto* previous = CurrentSystemInvocation_;
    CurrentSystemInvocation_ = &invocation;
    try {
        Systems_[index].Callback(*this);
    } catch (...) {
        invocation.Error = std::current_exception();
        CurrentSystemInvocation_ = previous;
        throw;
    }
    CurrentSystemInvocation_ = previous;
}

void World::DiscardSystemCommands(std::vector<SystemInvocation>& invocations) {
    for (auto& invocation: invocations) {
        RollbackDeferredStructuralCommands(invocation.Commands);
        for (auto& batch: invocation.WorkerBatches) {
            RollbackDeferredStructuralCommands(batch.Commands);
        }
        invocation.WorkerBatches.clear();
    }
    std::scoped_lock lock{DeferredStructuralMutex_};
    for (auto iterator = PlannedDeferredEntityReservations_.begin();
         iterator != PlannedDeferredEntityReservations_.end();) {
        if (!iterator->first.has_value()) {
            ++iterator;
            continue;
        }
        for (auto& [producerOrder, reservations]: iterator->second) {
            static_cast<void>(producerOrder);
            ReleaseUnusedDeferredEntityReservationsLocked(reservations, 0);
        }
        iterator = PlannedDeferredEntityReservations_.erase(iterator);
    }
    std::ranges::sort(FreeEntityIndices_);
}

void World::PublishSystemCommands(std::vector<SystemInvocation>& invocations) {
    std::scoped_lock lock{DeferredStructuralMutex_};
    if (OutstandingDeferredReservationBufferCount_ != 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "ECS deferred reservation buffers must be submitted or discarded before commit");
    }
    std::size_t batchCount = DeferredStructuralBatches_.size();
    for (const auto& invocation: invocations) {
        batchCount += invocation.WorkerBatches.size() + (invocation.Commands.empty() ? 0 : 1);
    }
    DeferredStructuralBatches_.reserve(batchCount);
    for (auto& invocation: invocations) {
        if (!invocation.Commands.empty()) {
            AppendDeferredCommandBatch({
                    .Source = DeferredCommandBatch::ESource::SYSTEM,
                    .SystemOrder = *invocation.StructuralOrder,
                    .ProducerOrder = *invocation.StructuralOrder,
                    .Commands = std::move(invocation.Commands),
            });
        }
        for (auto& batch: invocation.WorkerBatches) {
            AppendDeferredCommandBatch(std::move(batch));
        }
        invocation.WorkerBatches.clear();
    }
}

void World::RunSystemsImpl(NCommon::TaskSystem* taskSystem) {
    const SystemExecutionGraph graph = BuildSystemExecutionGraph(taskSystem != nullptr);
    std::vector<SystemInvocation> invocations(Systems_.size());
    std::vector<NCommon::TaskHandle> tasks(Systems_.size());
    std::vector<NCommon::TaskHandle> submitted;
    submitted.reserve(Systems_.size());
    if (IsRunningSystems_.exchange(true)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "ECS systems are already running");
    }
    try {
        for (const std::size_t index: graph.Order) {
            invocations[index].Owner = this;
            invocations[index].StructuralOrder = Systems_[index].StructuralOrder;
            invocations[index].Id = ++NextSystemInvocationId_;
        }
        std::exception_ptr error;
        if (taskSystem == nullptr) {
            try {
                for (const std::size_t index: graph.Order) {
                    ExecuteSystem(index, invocations[index]);
                }
            } catch (...) {
                error = std::current_exception();
            }
        } else {
            try {
                for (const std::size_t index: graph.Order) {
                    std::vector<NCommon::TaskHandle> dependencies;
                    dependencies.reserve(graph.Dependencies[index].size());
                    for (const std::size_t dependency: graph.Dependencies[index]) {
                        dependencies.push_back(tasks[dependency]);
                    }
                    tasks[index] = taskSystem->Submit(
                            [&, index](NCommon::TaskContext&) { ExecuteSystem(index, invocations[index]); },
                            dependencies);
                    submitted.push_back(tasks[index]);
                }
            } catch (...) {
                error = std::current_exception();
            }
            for (const auto& task: submitted) {
                taskSystem->Wait(task);
            }
        }
        if (error == nullptr) {
            for (const std::size_t index: graph.Order) {
                if (invocations[index].Error != nullptr) {
                    error = invocations[index].Error;
                    break;
                }
            }
        }
        if (error != nullptr) {
            std::rethrow_exception(error);
        }
        PublishSystemCommands(invocations);
        auto* previous = CommittingWorld_;
        CommittingWorld_ = this;
        try {
            ApplyDeferredStructuralChanges();
        } catch (...) {
            CommittingWorld_ = previous;
            throw;
        }
        CommittingWorld_ = previous;
    } catch (...) {
        DiscardSystemCommands(invocations);
        IsRunningSystems_ = false;
        throw;
    }
    IsRunningSystems_ = false;
}

void World::ApplyDeferredStructuralChanges() {
    if (IsRunningSystems_ && CommittingWorld_ != this) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "ECS deferred structural changes cannot be explicitly committed during system execution");
    }

    std::vector<std::unique_ptr<IDeferredCommand>> commands;

    {
        std::scoped_lock lock{DeferredStructuralMutex_};
        if (OutstandingDeferredReservationBufferCount_ != 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "ECS deferred reservation buffers must be submitted or discarded before commit");
        }
        for (auto& [systemOrder, phase]: PlannedDeferredEntityReservations_) {
            static_cast<void>(systemOrder);
            for (auto& [producerOrder, reservations]: phase) {
                static_cast<void>(producerOrder);
                ReleaseUnusedDeferredEntityReservationsLocked(reservations, 0);
            }
        }
        PlannedDeferredEntityReservations_.clear();
        std::ranges::sort(DeferredStructuralBatches_,
                          [](const DeferredCommandBatch& lhs, const DeferredCommandBatch& rhs) {
                              if (lhs.Source != rhs.Source) {
                                  return lhs.Source < rhs.Source;
                              }
                              if (lhs.SystemOrder != rhs.SystemOrder) {
                                  return lhs.SystemOrder < rhs.SystemOrder;
                              }
                              if (lhs.IsWorker != rhs.IsWorker) {
                                  return lhs.IsWorker < rhs.IsWorker;
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

    std::size_t nextCommand = 0;

    try {
        for (auto& command: commands) {
            command->Apply(*this);
            ++nextCommand;
        }
    } catch (...) {
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

    PendingDeferredEntityCreateCount_ = 0;
    PendingDeferredAppendEntities_.clear();
    ReservedDeferredEntityCount_ = 0;
    std::ranges::sort(FreeEntityIndices_);
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
    if (IsRunningSystems_ && CommittingWorld_ != this) {
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
    if (IsRunningSystems_) {
        ValidateSystemDeferredCommandCollectionLocked();
        GetCurrentSystemInvocation()->Commands.push_back(std::move(command));
        return;
    }
    DeferredStructuralCommands_.push_back(std::move(command));
}

void World::ValidateSystemDeferredCommandCollectionLocked() const {
    const auto* invocation = GetCurrentSystemInvocation();
    if (invocation == nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "ECS parallel workers must use producer-local deferred command buffers");
    }
    if (!invocation->StructuralOrder.has_value()) {
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
