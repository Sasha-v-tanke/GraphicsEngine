#pragma once

#include "entity.h"
#include "hierarchy.h"
#include "system_access.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <GraphicsEngine/lib/common/change_tracker.h>
#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>

namespace NCommon {
class TaskSystem;
}

namespace NEcs::NInternal {
class WorldExecutionAccess;
class ScopedWorldAccess;
class WorldChangeAccess;
struct ScopedSystem;
struct ResolvedBatch;
} // namespace NEcs::NInternal

namespace NEcs {

class World {
public:
    using SystemCallback = std::function<void(World&)>;

    struct DeferredProducer {
        std::uint32_t Order = 0;
        std::uint32_t EntityCreateCount = 0;
        std::optional<std::uint32_t> SystemOrder = std::nullopt;
    };

    class DeferredStructuralCommandBuffer;

    enum class EStructuralState {
        PENDING_CREATE,
        ALIVE,
        PENDING_DESTROY,
        DESTROYED
    };

    // User structural intake. Commands publish only at a quiescent commit boundary.
    // EntityReference carries the owning World identity; legacy immediate/Defer APIs retain their contracts.
    [[nodiscard]] EntityReference QueueCreateEntity();
    [[nodiscard]] EntityReference QueueCreateEntity(DeferredStructuralCommandBuffer& buffer);
    void QueueDestroyEntity(EntityReference entity);
    static void QueueDestroyEntity(DeferredStructuralCommandBuffer& buffer, EntityReference entity);
    void QueueReparent(EntityReference child, EntityReference parent, EReparentMode mode = EReparentMode::KEEP_LOCAL);
    static void QueueReparent(DeferredStructuralCommandBuffer& buffer,
                              EntityReference child,
                              EntityReference parent,
                              EReparentMode mode = EReparentMode::KEEP_LOCAL);
    void QueueSetEnabled(EntityReference entity, bool enabled);
    static void QueueSetEnabled(DeferredStructuralCommandBuffer& buffer, EntityReference entity, bool enabled);
    [[nodiscard]] EStructuralState GetStructuralState(EntityReference entity) const;
    [[nodiscard]] bool IsEntityEnabled(EntityReference entity) const;
    // Host-only offline commit. Runtime SystemRun finalization uses the same commit automatically.
    void CommitPending();
    [[nodiscard]] bool IsExecutionPoisoned() const noexcept {
        return ExecutionPoisoned_;
    }

    template<typename T, typename... TArgs>
    void QueueAddComponent(EntityReference entity, TArgs&&... args) {
        auto command =
                std::make_unique<DeferredAddComponentCommand<T, std::decay_t<TArgs>...>>(entity.Identity,
                                                                                         std::forward<TArgs>(args)...);
        QueueUserCommand(entity, std::move(command));
    }
    template<typename T, typename... TArgs>
    static void QueueAddComponent(DeferredStructuralCommandBuffer& buffer, EntityReference entity, TArgs&&... args) {
        auto command =
                std::make_unique<DeferredAddComponentCommand<T, std::decay_t<TArgs>...>>(entity.Identity,
                                                                                         std::forward<TArgs>(args)...);
        QueueUserCommand(buffer, entity, std::move(command));
    }
    template<typename T>
    void QueueRemoveComponent(EntityReference entity) {
        QueueUserCommand(entity, std::make_unique<DeferredRemoveComponentCommand<T>>(entity.Identity));
    }
    template<typename T>
    static void QueueRemoveComponent(DeferredStructuralCommandBuffer& buffer, EntityReference entity) {
        QueueUserCommand(buffer, entity, std::make_unique<DeferredRemoveComponentCommand<T>>(entity.Identity));
    }
    template<typename T>
    void QueueSetComponentEnabled(EntityReference entity, bool enabled) {
        QueueUserCommand(entity, std::make_unique<DeferredEnabledCommand>(entity.Identity, typeid(T), enabled));
    }
    template<typename T>
    static void
    QueueSetComponentEnabled(DeferredStructuralCommandBuffer& buffer, EntityReference entity, bool enabled) {
        QueueUserCommand(buffer, entity, std::make_unique<DeferredEnabledCommand>(entity.Identity, typeid(T), enabled));
    }
    template<typename T>
    [[nodiscard]] bool IsComponentEnabled(EntityReference entity) const {
        ValidateUnscopedAccess();
        ValidateUserIdentity(entity);
        return HasComponent<T>(entity.Identity) && !IsComponentDisabled(entity.Identity, typeid(T));
    }

    World();
    ~World();

    // References qualify generational Entity handles with this World's stable lifetime identity.
    [[nodiscard]] EntityReference GetEntityReference(Entity entity) const noexcept;
    [[nodiscard]] TopologySnapshot AcquireTopologySnapshot() const;
    void SetLocalTransform(EntityReference entity, const NMath::Mat4& local);
    void DeferReparent(EntityReference child, EntityReference parent, EReparentMode mode = EReparentMode::KEEP_LOCAL);
    static void DeferReparent(DeferredStructuralCommandBuffer& buffer,
                              EntityReference child,
                              EntityReference parent,
                              EReparentMode mode = EReparentMode::KEEP_LOCAL);

    Entity CreateEntity();
    void DestroyEntity(Entity entity);

    Entity DeferCreateEntity();
    Entity DeferCreateEntity(DeferredStructuralCommandBuffer& buffer);
    void DeferDestroyEntity(Entity entity);
    static void DeferDestroyEntity(DeferredStructuralCommandBuffer& buffer, Entity entity);

    [[nodiscard]] bool IsAlive(Entity entity) const noexcept;
    [[nodiscard]] std::size_t GetAliveEntityCount() const noexcept;

    template<typename T, typename... TArgs>
    T& AddComponent(Entity entity, TArgs&&... args) {
        ValidateStructuralWriteAllowed();
        StructuralWriteGuard structuralGuard{*this};
        ValidateAlive(entity);
        auto& storage = GetOrCreateStorage<T>();
        TrackComponentAccess(typeid(T));
        return storage.Emplace(entity.Index, std::forward<TArgs>(args)...);
    }

    template<typename T, typename... TArgs>
    void DeferAddComponent(Entity entity, TArgs&&... args) {
        PushDeferredCommand(
                std::make_unique<DeferredAddComponentCommand<T, std::decay_t<TArgs>...>>(entity,
                                                                                         std::forward<TArgs>(args)...));
    }

    template<typename T, typename... TArgs>
    static void DeferAddComponent(DeferredStructuralCommandBuffer& buffer, Entity entity, TArgs&&... args) {
        PushDeferredCommand(
                buffer,
                std::make_unique<DeferredAddComponentCommand<T, std::decay_t<TArgs>...>>(entity,
                                                                                         std::forward<TArgs>(args)...));
    }

    template<typename T>
    [[nodiscard]] bool HasComponent(Entity entity) const {
        ValidateUnscopedAccess();
        if (!IsAlive(entity)) {
            return false;
        }

        const auto* storage = FindStorage<T>();
        return storage != nullptr && storage->Contains(entity.Index);
    }

    template<typename T>
    [[nodiscard]] T& GetComponent(Entity entity) {
        ValidateUnscopedAccess();
        ValidateAlive(entity);
        auto* storage = FindStorage<T>();

        if (storage == nullptr || !storage->Contains(entity.Index)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_FOUND, "ECS component does not exist");
        }

        TrackComponentAccess(typeid(T));
        return storage->Get(entity.Index);
    }

    template<typename T>
    [[nodiscard]] const T& GetComponent(Entity entity) const {
        ValidateUnscopedAccess();
        ValidateAlive(entity);
        const auto* storage = FindStorage<T>();

        if (storage == nullptr || !storage->Contains(entity.Index)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_FOUND, "ECS component does not exist");
        }

        return storage->Get(entity.Index);
    }

    template<typename T>
    bool RemoveComponent(Entity entity) {
        ValidateStructuralWriteAllowed();
        StructuralWriteGuard structuralGuard{*this};

        if (!IsAlive(entity)) {
            return false;
        }

        auto* storage = FindStorage<T>();
        if (storage != nullptr && storage->Contains(entity.Index)) {
            TrackComponentAccess(typeid(T));
        }
        const bool removed = storage != nullptr && storage->RemoveComponent(entity.Index);
        if (removed) {
            if (const auto disabled = m_disabledComponents.find(typeid(T)); disabled != m_disabledComponents.end()) {
                disabled->second.erase(entity.Index);
            }
        }
        return removed;
    }

    template<typename T>
    void DeferRemoveComponent(Entity entity) {
        PushDeferredCommand(std::make_unique<DeferredRemoveComponentCommand<T>>(entity));
    }

    template<typename T>
    static void DeferRemoveComponent(DeferredStructuralCommandBuffer& buffer, Entity entity) {
        PushDeferredCommand(buffer, std::make_unique<DeferredRemoveComponentCommand<T>>(entity));
    }

    template<typename... TComponents, typename TCallback>
    void Query(TCallback&& callback) {
        ValidateUnscopedAccess();
        std::vector<Entity> entities;
        entities.reserve(Entities_.size());

        for (std::uint32_t index = 0; index < Entities_.size(); ++index) {
            if (Entities_[index].Alive) {
                entities.push_back({
                        .Index = index,
                        .Generation = Entities_[index].Generation,
                });
            }
        }

        for (Entity entity: entities) {
            if ((!HasComponent<TComponents>(entity) || ...)) {
                continue;
            }

            std::invoke(callback, entity, GetComponent<TComponents>(entity)...);
        }
    }

    template<typename... TComponents, typename TCallback>
    void Query(TCallback&& callback) const {
        ValidateUnscopedAccess();
        std::vector<Entity> entities;
        entities.reserve(Entities_.size());

        for (std::uint32_t index = 0; index < Entities_.size(); ++index) {
            if (Entities_[index].Alive) {
                entities.push_back({
                        .Index = index,
                        .Generation = Entities_[index].Generation,
                });
            }
        }

        for (Entity entity: entities) {
            if ((!HasComponent<TComponents>(entity) || ...)) {
                continue;
            }

            std::invoke(callback, entity, GetComponent<TComponents>(entity)...);
        }
    }

    void RegisterSystem(SystemAccessList access, SystemCallback callback);
    void RegisterSystem(SystemAccessList access, SystemCallback callback, std::uint32_t structuralOrder);
    void RegisterSystem(SystemAccessList access,
                        SystemCallback callback,
                        SystemSchedule schedule,
                        std::optional<std::uint32_t> structuralOrder = std::nullopt);

    [[nodiscard]] const std::vector<SystemAccess>& GetSystemAccess(std::size_t index) const;
    [[nodiscard]] std::size_t GetSystemCount() const noexcept;
    void BeginDeferredStructuralPhase(std::vector<DeferredProducer> producers);
    [[nodiscard]] DeferredStructuralCommandBuffer CreateDeferredStructuralCommandBuffer(std::uint32_t producerOrder);
    void SubmitDeferredStructuralCommands(DeferredStructuralCommandBuffer&& buffer);
    void RunSystems();
    void RunSystems(NCommon::TaskSystem& taskSystem);
    void ApplyDeferredStructuralChanges();

private:
    class StructuralWriteGuard final {
    public:
        explicit StructuralWriteGuard(World& world);
        ~StructuralWriteGuard();
        StructuralWriteGuard(const StructuralWriteGuard&) = delete;
        StructuralWriteGuard& operator=(const StructuralWriteGuard&) = delete;

    private:
        World& m_world;
    };
    struct EntityState {
        std::uint32_t Generation = 0;
        bool Alive = false;
        bool Reserved = false;
    };

    struct DeferredEntityReservation {
        Entity Identity;
        bool ReusesFreeSlot = false;
    };

    enum class ECommandKind {
        CREATE,
        DESTROY,
        ADD,
        REMOVE,
        REPARENT,
        ENABLE
    };
    struct CommandDescription {
        ECommandKind Kind = ECommandKind::DESTROY;
        Entity Identity;
        std::uint64_t WorldId = 0;
        std::type_index Component = typeid(void);
        bool Strict = false;
        EntityReference Parent = {};
        EReparentMode ReparentMode = EReparentMode::KEEP_LOCAL;
        bool Enabled = true;
    };

    class IDeferredCommand {
    public:
        virtual ~IDeferredCommand() = default;
        CommandDescription Description;

        virtual void Apply(World& world) = 0;
        [[nodiscard]] virtual std::uint32_t GetReservedEntityCount() const noexcept;
        virtual void RollbackReservation(World& world);
    };

public:
    class DeferredStructuralCommandBuffer {
    public:
        DeferredStructuralCommandBuffer(DeferredStructuralCommandBuffer&&) noexcept;
        DeferredStructuralCommandBuffer& operator=(DeferredStructuralCommandBuffer&&) noexcept;
        ~DeferredStructuralCommandBuffer();

        DeferredStructuralCommandBuffer(const DeferredStructuralCommandBuffer&) = delete;
        DeferredStructuralCommandBuffer& operator=(const DeferredStructuralCommandBuffer&) = delete;

    private:
        friend class World;

        struct State;

        DeferredStructuralCommandBuffer(World& owner, std::uint32_t producerOrder);

        std::unique_ptr<State> State_;
    };

private:
    struct DeferredCommandBatch {
        enum class ESource : std::uint8_t {
            SYSTEM,
            EXTERNAL,
        };

        ESource Source = ESource::EXTERNAL;
        std::uint32_t SystemOrder = 0;
        bool IsWorker = false;
        std::uint32_t ProducerOrder = 0;
        std::uint64_t SubmissionOrder = 0;
        std::vector<std::unique_ptr<IDeferredCommand>> Commands;
    };

    class DeferredReparentCommand final: public IDeferredCommand {
    public:
        DeferredReparentCommand(EntityReference child, EntityReference parent, EReparentMode mode);
        void Apply(World& world) override;

    private:
        EntityReference m_child;
        EntityReference m_parent;
        EReparentMode m_mode;
    };

    class DeferredCreateEntityCommand final: public IDeferredCommand {
    public:
        DeferredCreateEntityCommand(Entity entity, bool reusedFreeSlot);

        void Apply(World& world) override;
        [[nodiscard]] std::uint32_t GetReservedEntityCount() const noexcept override;
        void RollbackReservation(World& world) override;

    private:
        Entity Entity_;
        bool ReusedFreeSlot_ = false;
    };

    class DeferredDestroyEntityCommand final: public IDeferredCommand {
    public:
        explicit DeferredDestroyEntityCommand(Entity entity);

        void Apply(World& world) override;

    private:
        Entity Entity_;
    };

    template<typename T, typename... TArgs>
    class DeferredAddComponentCommand final: public IDeferredCommand {
    public:
        template<typename... TValues>
        DeferredAddComponentCommand(Entity entity, TValues&&... args)
            : Entity_(entity)
            , Args_(std::forward<TValues>(args)...) {
            this->Description = {.Kind = ECommandKind::ADD, .Identity = entity, .Component = typeid(T)};
        }

        void Apply(World& world) override {
            if (!world.IsAlive(Entity_)) {
                return;
            }

            std::apply([&](auto&... args) { world.AddComponent<T>(Entity_, std::move(args)...); }, Args_);
        }

    private:
        Entity Entity_;
        std::tuple<TArgs...> Args_;
    };

    template<typename T>
    class DeferredRemoveComponentCommand final: public IDeferredCommand {
    public:
        explicit DeferredRemoveComponentCommand(Entity entity)
            : Entity_(entity) {
            this->Description = {.Kind = ECommandKind::REMOVE, .Identity = entity, .Component = typeid(T)};
        }

        void Apply(World& world) override {
            static_cast<void>(world.RemoveComponent<T>(Entity_));
        }

    private:
        Entity Entity_;
    };

    class DeferredEnabledCommand final: public IDeferredCommand {
    public:
        DeferredEnabledCommand(Entity entity, std::type_index component, bool enabled);
        void Apply(World& world) override;
    };

    class IComponentStorage {
    public:
        virtual ~IComponentStorage() = default;

        virtual void RemoveEntity(std::uint32_t entityIndex) = 0;
        [[nodiscard]] virtual bool Contains(std::uint32_t entityIndex) const = 0;
    };

    template<typename T>
    class ComponentStorage final: public IComponentStorage {
    public:
        template<typename... TArgs>
        T& Emplace(std::uint32_t entityIndex, TArgs&&... args) {
            Values_.erase(entityIndex);
            auto [iterator, inserted] = Values_.try_emplace(entityIndex, std::forward<TArgs>(args)...);
            static_cast<void>(inserted);
            return iterator->second;
        }

        [[nodiscard]] bool Contains(std::uint32_t entityIndex) const override {
            return Values_.contains(entityIndex);
        }

        [[nodiscard]] T& Get(std::uint32_t entityIndex) {
            return Values_.at(entityIndex);
        }

        [[nodiscard]] const T& Get(std::uint32_t entityIndex) const {
            return Values_.at(entityIndex);
        }

        bool RemoveComponent(std::uint32_t entityIndex) {
            return Values_.erase(entityIndex) > 0;
        }

        void RemoveEntity(std::uint32_t entityIndex) override {
            Values_.erase(entityIndex);
        }

    private:
        std::unordered_map<std::uint32_t, T> Values_;
    };

    template<typename T>
    [[nodiscard]] ComponentStorage<T>* FindStorage() {
        const auto iterator = ComponentStorageByType_.find(typeid(T));

        if (iterator == ComponentStorageByType_.end()) {
            return nullptr;
        }

        return static_cast<ComponentStorage<T>*>(iterator->second.get());
    }

    template<typename T>
    [[nodiscard]] const ComponentStorage<T>* FindStorage() const {
        const auto iterator = ComponentStorageByType_.find(typeid(T));

        if (iterator == ComponentStorageByType_.end()) {
            return nullptr;
        }

        return static_cast<const ComponentStorage<T>*>(iterator->second.get());
    }

    template<typename T>
    ComponentStorage<T>& GetOrCreateStorage() {
        const std::type_index componentType = typeid(T);
        auto iterator = ComponentStorageByType_.find(componentType);

        if (iterator == ComponentStorageByType_.end()) {
            iterator = ComponentStorageByType_.emplace(componentType, std::make_unique<ComponentStorage<T>>()).first;
        }

        return *static_cast<ComponentStorage<T>*>(iterator->second.get());
    }

    void CommitUserInitialization();
    void ValidateUserIdentity(EntityReference entity) const;
    [[nodiscard]] bool IsPendingIdentity(Entity entity) const;
    [[nodiscard]] bool IsComponentDisabled(Entity entity, std::type_index component) const;
    void QueueUserCommand(EntityReference entity, std::unique_ptr<IDeferredCommand> command);
    static void QueueUserCommand(DeferredStructuralCommandBuffer& buffer,
                                 EntityReference entity,
                                 std::unique_ptr<IDeferredCommand> command);
    static void ValidateUserBatch(const std::vector<std::unique_ptr<IDeferredCommand>>& commands,
                                  const CommandDescription& incoming);
    void PreflightStructuralCommands(const std::vector<std::unique_ptr<IDeferredCommand>>& commands) const;
    void ValidateAlive(Entity entity) const;
    void ValidateSystemIndex(std::size_t index) const;
    struct SystemExecutionGraph {
        std::vector<std::size_t> Order;
        std::vector<std::vector<std::size_t>> Dependencies;
    };

    [[nodiscard]] SystemExecutionGraph BuildSystemExecutionGraph(bool requireSchedules) const;
    friend class NInternal::WorldExecutionAccess;
    friend class NInternal::ScopedWorldAccess;
    friend class NInternal::WorldChangeAccess;
    NCommon::ChangeDomainKey GetChangeDomain(std::type_index type, bool entityRevisions = false);
    void TrackComponentAccess(std::type_index type);
    struct SystemRunState;
    std::shared_ptr<SystemRunState> ActiveSystemRun_;
    std::atomic_bool ExecutionPoisoned_ = false;
    std::atomic_bool m_isCommitting = false;
    bool m_commitRejectedBeforePublication = false;
    struct SystemInvocation;
    [[nodiscard]] SystemInvocation* GetCurrentSystemInvocation() const noexcept;
    void RunSystemsImpl();
    void ExecuteSystem(std::size_t index, SystemInvocation& invocation);
    void DiscardSystemCommands(std::vector<SystemInvocation>& invocations);
    void PublishSystemCommands(std::vector<SystemInvocation>& invocations);
    void DiscardPublishedCommands();
    static thread_local SystemInvocation* CurrentSystemInvocation_;
    static thread_local World* CommittingWorld_;
    void ValidateStructuralWriteAllowed() const;
    void ValidateUnscopedAccess() const;
    void PrepareScopedInvocations(std::vector<SystemInvocation>& invocations);
    void CollectScopedDirty(std::vector<SystemInvocation>& invocations) noexcept;
    void ValidateDeferredStructuralCommandBuffer(const DeferredStructuralCommandBuffer& buffer) const;
    void ValidateSystemDeferredCommandCollectionLocked() const;
    void PushDeferredCommand(std::unique_ptr<IDeferredCommand> command);
    static void PushDeferredCommand(DeferredStructuralCommandBuffer& buffer, std::unique_ptr<IDeferredCommand> command);
    void AppendDeferredCommandBatch(DeferredCommandBatch batch);
    void RollbackDeferredStructuralCommands(std::vector<std::unique_ptr<IDeferredCommand>>& commands);
    void ReleaseDeferredEntityReservationLocked(const DeferredEntityReservation& reservation);
    void ReleaseUnusedDeferredEntityReservationsLocked(std::vector<DeferredEntityReservation>& reservations,
                                                       std::size_t firstUnused);
    Entity ReserveDeferredEntityLocked(std::vector<std::unique_ptr<IDeferredCommand>>& commands);
    [[nodiscard]] DeferredEntityReservation ReserveDeferredEntityLocked();
    [[nodiscard]] std::optional<std::uint32_t> FindPendingDeferredAppendGeneration(std::uint32_t index) const noexcept;
    [[nodiscard]] bool HasPendingDeferredAppendReservation(std::uint32_t index) const noexcept;
    void RemovePendingDeferredAppendReservation(std::uint32_t index);
    [[nodiscard]] std::uint32_t FindNextDeferredAppendIndex() const noexcept;

    std::shared_ptr<NInternal::TopologyState> m_topology;
    NCommon::ChangeTracker m_changes;
    std::map<std::type_index, NCommon::ChangeDomainKey> m_changeDomains;
    std::set<std::type_index> m_entityRevisionTypes;
    NCommon::DirtySummary m_pendingChanges;
    std::vector<const NCommon::DirtySummary*> m_changeInputs;
    std::vector<EntityState> Entities_;
    std::vector<std::uint32_t> FreeEntityIndices_;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentStorage>> ComponentStorageByType_;
    std::set<std::uint32_t> m_disabledEntities;
    std::map<std::type_index, std::set<std::uint32_t>> m_disabledComponents;
    mutable std::mutex DeferredStructuralMutex_;
    std::vector<std::unique_ptr<IDeferredCommand>> DeferredStructuralCommands_;
    std::vector<DeferredCommandBatch> DeferredStructuralBatches_;
    std::vector<Entity> PendingDeferredAppendEntities_;
    std::map<std::optional<std::uint32_t>, std::unordered_map<std::uint32_t, std::vector<DeferredEntityReservation>>>
            PlannedDeferredEntityReservations_;
    std::uint32_t PendingDeferredEntityCreateCount_ = 0;
    std::uint32_t OutstandingDeferredReservationBufferCount_ = 0;
    std::uint32_t m_outstandingDeferredBuffers = 0;
    std::uint32_t ReservedDeferredEntityCount_ = 0;
    std::uint32_t NextNewEntityGeneration_ = 0;
    std::uint64_t NextDeferredBatchSubmissionOrder_ = 0;
    std::shared_ptr<bool> LifetimeToken_;
    std::atomic_bool IsRunningSystems_ = false;
    std::uint64_t NextSystemInvocationId_ = 0;

    std::vector<std::type_index> ScopedDirtyTypes_;
    bool ScopedDirtyAll_ = false;

    struct RegisteredSystem {
        SystemAccessList Access;
        SystemCallback Callback;
        std::optional<std::uint32_t> StructuralOrder;
        std::optional<SystemSchedule> Schedule = std::nullopt;
        std::shared_ptr<NInternal::ScopedSystem> Scoped = {};
    };

    std::vector<RegisteredSystem> Systems_;
};

} // namespace NEcs
