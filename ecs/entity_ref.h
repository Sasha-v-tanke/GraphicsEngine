#pragma once

#include "hierarchy.h"
#include "world.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace NEcs {

// Non-owning identity. World teardown requires quiescence; this handle does not pin World or components.
// Component references expire at the end of the owner phase/invocation or a structural commit.
class EntityRef final {
public:
    EntityRef() = default;

    template<typename T, typename... TArgs>
    EntityRef AddComponent(TArgs&&... args) {
        static_assert(std::is_same_v<T, std::remove_cvref_t<T>>);
        World& world = GetWorld();
        EntityRef result = world.GetEntityRef(m_reference);
        world.QueueAddComponent<T>(m_reference, std::forward<TArgs>(args)...);
        return result;
    }

    template<typename T>
    [[nodiscard]] T& GetComponent() {
        if (T* component = TryGetComponent<T>()) {
            return *component;
        }
        ThrowMissingComponent();
    }

    template<typename T>
    [[nodiscard]] const std::remove_const_t<T>& GetComponent() const {
        if (const auto* component = TryGetComponent<T>()) {
            return *component;
        }
        ThrowMissingComponent();
    }

    // nullptr means absent or pending-create. Stale/expired identities and denied access throw.
    template<typename T>
    [[nodiscard]] T* TryGetComponent() {
        static_assert(!std::is_reference_v<T> && !std::is_volatile_v<T>);
        return GetWorld().ResolveEntityRefComponent<std::remove_const_t<T>>(m_reference, !std::is_const_v<T>);
    }

    template<typename T>
    [[nodiscard]] const std::remove_const_t<T>* TryGetComponent() const {
        static_assert(!std::is_reference_v<T> && !std::is_volatile_v<T>);
        return GetWorld().ResolveEntityRefComponent<std::remove_const_t<T>>(m_reference, false);
    }

    template<typename T>
    [[nodiscard]] bool HasComponent() const {
        return TryGetComponent<T>() != nullptr;
    }

    template<typename T>
    void RemoveComponent() {
        static_assert(std::is_same_v<T, std::remove_cvref_t<T>>);
        GetWorld().QueueRemoveComponent<T>(m_reference);
    }

    void Destroy();
    // Pending refers to unpublished creation. A pending destroy remains alive until commit.
    [[nodiscard]] bool IsPending() const;
    [[nodiscard]] bool IsAlive() const;
    // Identity metadata only; it grants no access and is not a persistent GUID.
    [[nodiscard]] EntityReference GetReference() const noexcept;

private:
    friend class World;
    EntityRef(World& world, EntityReference reference);
    [[nodiscard]] World& GetWorld() const;
    [[noreturn]] void ThrowMissingComponent() const;

private:
    World* m_world = nullptr;
    EntityReference m_reference = {};
    std::weak_ptr<bool> m_lifetime;
    std::weak_ptr<const NInternal::EntityAccessScope> m_scope;
    bool m_scopeBound = false;
};

} // namespace NEcs
