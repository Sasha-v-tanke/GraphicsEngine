#pragma once

#include "entity.h"
#include "system_access.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>

namespace NEcs {

class World {
public:
    using SystemCallback = std::function<void(World&)>;

    class DeferredStructuralCommandBuffer;

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
        ValidateAlive(entity);
        auto& storage = GetOrCreateStorage<T>();
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
        if (!IsAlive(entity)) {
            return false;
        }

        const auto* storage = FindStorage<T>();
        return storage != nullptr && storage->Contains(entity.Index);
    }

    template<typename T>
    [[nodiscard]] T& GetComponent(Entity entity) {
        ValidateAlive(entity);
        auto* storage = FindStorage<T>();

        if (storage == nullptr || !storage->Contains(entity.Index)) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_FOUND, "ECS component does not exist");
        }

        return storage->Get(entity.Index);
    }

    template<typename T>
    [[nodiscard]] const T& GetComponent(Entity entity) const {
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

        if (!IsAlive(entity)) {
            return false;
        }

        auto* storage = FindStorage<T>();
        return storage != nullptr && storage->RemoveComponent(entity.Index);
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

    [[nodiscard]] const std::vector<SystemAccess>& GetSystemAccess(std::size_t index) const;
    [[nodiscard]] std::size_t GetSystemCount() const noexcept;
    [[nodiscard]] static DeferredStructuralCommandBuffer
    CreateDeferredStructuralCommandBuffer(std::uint32_t producerOrder);
    void SubmitDeferredStructuralCommands(DeferredStructuralCommandBuffer&& buffer);
    void RunSystems();
    void ApplyDeferredStructuralChanges();

private:
    struct EntityState {
        std::uint32_t Generation = 0;
        bool Alive = false;
        bool Reserved = false;
    };

    class IDeferredCommand {
    public:
        virtual ~IDeferredCommand() = default;

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

        explicit DeferredStructuralCommandBuffer(std::uint32_t producerOrder);

        std::unique_ptr<State> State_;
    };

private:
    struct DeferredCommandBatch {
        std::uint32_t ProducerOrder = 0;
        std::uint64_t SubmissionOrder = 0;
        std::vector<std::unique_ptr<IDeferredCommand>> Commands;
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
        }

        void Apply(World& world) override {
            static_cast<void>(world.RemoveComponent<T>(Entity_));
        }

    private:
        Entity Entity_;
    };

    class IComponentStorage {
    public:
        virtual ~IComponentStorage() = default;

        virtual void RemoveEntity(std::uint32_t entityIndex) = 0;
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

        [[nodiscard]] bool Contains(std::uint32_t entityIndex) const {
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

    void ValidateAlive(Entity entity) const;
    void ValidateSystemIndex(std::size_t index) const;
    void ValidateStructuralWriteAllowed() const;
    void PushDeferredCommand(std::unique_ptr<IDeferredCommand> command);
    static void PushDeferredCommand(DeferredStructuralCommandBuffer& buffer, std::unique_ptr<IDeferredCommand> command);
    void AppendDeferredCommandBatch(DeferredCommandBatch batch);
    Entity ReserveDeferredEntityLocked(std::vector<std::unique_ptr<IDeferredCommand>>& commands);

    std::vector<EntityState> Entities_;
    std::vector<std::uint32_t> FreeEntityIndices_;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentStorage>> ComponentStorageByType_;
    std::mutex DeferredStructuralMutex_;
    std::vector<std::unique_ptr<IDeferredCommand>> DeferredStructuralCommands_;
    std::vector<DeferredCommandBatch> DeferredStructuralBatches_;
    std::vector<std::unique_ptr<IDeferredCommand>> ActiveDeferredStructuralCommands_;
    std::uint32_t ReservedDeferredEntityCount_ = 0;
    std::uint32_t NextNewEntityGeneration_ = 0;
    std::uint64_t NextDeferredBatchSubmissionOrder_ = 0;
    bool IsRunningSystems_ = false;
    bool IsCollectingSystemDeferredStructuralCommands_ = false;
    bool IsApplyingDeferredStructuralChanges_ = false;

    struct RegisteredSystem {
        SystemAccessList Access;
        SystemCallback Callback;
    };

    std::vector<RegisteredSystem> Systems_;
};

} // namespace NEcs
