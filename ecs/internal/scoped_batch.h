#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <typeindex>
#include <vector>

#include <GraphicsEngine/ecs/world.h>
#include <ecs/internal/access_coordinator.h>

namespace NEcs::NInternal {

// Resolved against a frozen World at run admission. Chunk coordinates are entity
// slot indices, not archetype IDs; all explicit references include World/generation.
struct BatchScope {
    EAccessScope Kind = EAccessScope::WHOLE_STORAGE;
    std::uint64_t Begin = 0;
    std::uint64_t Count = 0;
    std::vector<EntityReference> Entities = {};

    static BatchScope Entity(EntityReference entity) {
        return {.Kind = EAccessScope::ENTITY, .Entities = {entity}};
    }
    static BatchScope EntitySet(std::vector<EntityReference> entities) {
        return {.Kind = EAccessScope::ENTITY_SET, .Entities = std::move(entities)};
    }
    static BatchScope Chunk(std::uint64_t begin, std::uint64_t count) {
        return {.Kind = EAccessScope::CHUNK, .Begin = begin, .Count = count};
    }
    static BatchScope Unknown() {
        return {.Kind = EAccessScope::UNKNOWN};
    }
};

struct ScopedComponentAccess {
    SystemAccess Access;
    BatchScope Scope;
    // Only typed factories construct component resolvers. Resolution precedes publication.
    void* (*Resolve)(World&, Entity) = nullptr;
};

struct ResolvedBatch {
    struct Entry {
        std::type_index Type = typeid(void);
        EntityReference Entity;
        void* Component = nullptr;
        bool Writable = false;
    };
    SystemAccessList Declarations;
    std::vector<Entry> Entries;
    std::atomic_bool Active = false;
    std::thread::id Thread;
};

// Views are callback/thread bound. Component references must not escape the
// callback; detached work never inherits access. A saved view rejects later use.
template<typename T, bool Writable>
class ScopedView {
public:
    using Component = std::conditional_t<Writable, T, const T>;
    [[nodiscard]] Component& Get(EntityReference entity) const {
        Check();
        for (const auto& entry: m_batch->Entries) {
            if (entry.Type == typeid(T) && entry.Entity == entity && (!Writable || entry.Writable)) {
                return *static_cast<Component*>(entry.Component);
            }
        }
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Component is outside the declared scope");
    }
    template<typename Callback>
    void ForEach(Callback&& callback) const {
        Check();
        for (const auto& entry: m_batch->Entries) {
            if (entry.Type == typeid(T) && (!Writable || entry.Writable)) {
                std::invoke(callback, entry.Entity, *static_cast<Component*>(entry.Component));
            }
        }
    }

private:
    explicit ScopedView(std::shared_ptr<const ResolvedBatch> batch)
        : m_batch(std::move(batch)) {
    }
    void Check() const {
        if (!m_batch->Active.load() || m_batch->Thread != std::this_thread::get_id()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Scoped view is outside its invocation");
        }
    }
    std::shared_ptr<const ResolvedBatch> m_batch;
    friend class ScopedBatchContext;
};

class ScopedBatchContext final: public NCommon::NonTransferable {
public:
    template<typename T>
    [[nodiscard]] ScopedView<T, false> Read() const {
        Validate(typeid(T), false);
        return ScopedView<T, false>{m_batch};
    }
    template<typename T>
    [[nodiscard]] ScopedView<T, true> Write() const {
        Validate(typeid(T), true);
        return ScopedView<T, true>{m_batch};
    }
    // Structural commands use the existing deterministic producer protocol. They
    // do not affect this invocation's frozen views and commit after all jobs retire.
    void DeferDestroy(EntityReference entity);
    template<typename T, typename... Args>
    void DeferAdd(EntityReference entity, Args&&... args) {
        ValidateEntity(entity);
        m_world.DeferAddComponent<T>(entity.Identity, std::forward<Args>(args)...);
    }
    template<typename T>
    void DeferRemove(EntityReference entity) {
        ValidateEntity(entity);
        m_world.DeferRemoveComponent<T>(entity.Identity);
    }

private:
    ScopedBatchContext(World& world, std::shared_ptr<ResolvedBatch> batch);
    void Validate(std::type_index type, bool write) const;
    void ValidateEntity(EntityReference entity) const;
    World& m_world;
    std::shared_ptr<ResolvedBatch> m_batch;
    friend class ScopedWorldAccess;
};

struct ScopedSystem {
    std::vector<ScopedComponentAccess> Access;
    std::function<void(ScopedBatchContext&)> Callback;
};

struct ScopedDirtySummary {
    bool AllComponents = false;
    std::vector<std::type_index> Types;
};

class ScopedWorldAccess {
public:
    static void Register(World& world,
                         std::vector<ScopedComponentAccess> access,
                         std::function<void(ScopedBatchContext&)> callback,
                         SystemSchedule schedule,
                         std::optional<std::uint32_t> structuralOrder = std::nullopt);
    // Quiescent owner only. Coarse component-type summary, including partial writes
    // on failure; no per-value versioning. Reserved for the future dirty pipeline.
    [[nodiscard]] static ScopedDirtySummary TakeDirtySummary(World& world);
    static std::shared_ptr<ResolvedBatch> Resolve(World& world, const ScopedSystem& system);
    static void Invoke(World& world, const ScopedSystem& system, const std::shared_ptr<ResolvedBatch>& batch);
    template<typename T>
    static void* ResolveComponent(World& world, Entity entity) {
        auto* storage = world.FindStorage<T>();
        return storage && storage->Contains(entity.Index) ? &storage->Get(entity.Index) : nullptr;
    }
};

template<typename T>
ScopedComponentAccess ScopedRead(BatchScope scope = BatchScope()) {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>);
    return {NEcs::Read<T>(), std::move(scope), &ScopedWorldAccess::ResolveComponent<T>};
}
template<typename T>
ScopedComponentAccess ScopedWrite(BatchScope scope = BatchScope()) {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>);
    return {NEcs::Write<T>(), std::move(scope), &ScopedWorldAccess::ResolveComponent<T>};
}

} // namespace NEcs::NInternal
