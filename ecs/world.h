#pragma once

#include "entity.h"
#include "system_access.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NEcs {

class World {
public:
    using SystemCallback = std::function<void(World&)>;

    Entity CreateEntity();
    void DestroyEntity(Entity entity);

    [[nodiscard]] bool IsAlive(Entity entity) const noexcept;
    [[nodiscard]] std::size_t GetAliveEntityCount() const noexcept;

    template<typename T, typename... TArgs>
    T& AddComponent(Entity entity, TArgs&&... args) {
        ValidateAlive(entity);
        auto& storage = GetOrCreateStorage<T>();
        return storage.Emplace(entity.Index, T(std::forward<TArgs>(args)...));
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
            throw std::out_of_range("ECS component does not exist");
        }

        return storage->Get(entity.Index);
    }

    template<typename T>
    [[nodiscard]] const T& GetComponent(Entity entity) const {
        ValidateAlive(entity);
        const auto* storage = FindStorage<T>();

        if (storage == nullptr || !storage->Contains(entity.Index)) {
            throw std::out_of_range("ECS component does not exist");
        }

        return storage->Get(entity.Index);
    }

    template<typename T>
    bool RemoveComponent(Entity entity) {
        if (!IsAlive(entity)) {
            return false;
        }

        auto* storage = FindStorage<T>();
        return storage != nullptr && storage->RemoveComponent(entity.Index);
    }

    template<typename... TComponents, typename TCallback>
    void Query(TCallback&& callback) {
        for (std::uint32_t index = 0; index < Entities_.size(); ++index) {
            const Entity entity{
                    .Index = index,
                    .Generation = Entities_[index].Generation,
            };

            if (!Entities_[index].Alive || (!HasComponent<TComponents>(entity) || ...)) {
                continue;
            }

            std::invoke(callback, entity, GetComponent<TComponents>(entity)...);
        }
    }

    void RegisterSystem(SystemAccessList access, SystemCallback callback);

    [[nodiscard]] const std::vector<SystemAccess>& GetSystemAccess(std::size_t index) const;
    [[nodiscard]] std::size_t GetSystemCount() const noexcept;
    void RunSystems();

private:
    struct EntityState {
        std::uint32_t Generation = 0;
        bool Alive = false;
    };

    class IComponentStorage {
    public:
        virtual ~IComponentStorage() = default;

        virtual void RemoveEntity(std::uint32_t entityIndex) = 0;
    };

    template<typename T>
    class ComponentStorage final: public IComponentStorage {
    public:
        T& Emplace(std::uint32_t entityIndex, T value) {
            Values_[entityIndex] = std::move(value);
            return Values_.at(entityIndex);
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

    std::vector<EntityState> Entities_;
    std::vector<std::uint32_t> FreeEntityIndices_;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentStorage>> ComponentStorageByType_;

    struct RegisteredSystem {
        SystemAccessList Access;
        SystemCallback Callback;
    };

    std::vector<RegisteredSystem> Systems_;
};

} // namespace NEcs
