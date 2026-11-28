#pragma once

#include "world.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace NEcs::NSystem {

enum class ESystemPhase {
    UPDATE,
};

template<typename... TSystems>
struct Before {};

template<typename... TSystems>
struct After {};

// Identity is stable within a given executable and never serialized. The
// World registry rejects duplicate IDs and structural-order collisions.
template<typename TSystem>
[[nodiscard]] std::uint64_t SystemId() noexcept {
    std::uint64_t id = 14695981039346656037ULL;
    for (const char value: std::string_view{typeid(TSystem).name()}) {
        id ^= static_cast<unsigned char>(value);
        id *= 1099511628211ULL;
    }
    return id;
}

template<typename... TSystems>
[[nodiscard]] std::vector<std::uint64_t> SystemIds(Before<TSystems...>*) {
    return {SystemId<TSystems>()...};
}
template<typename... TSystems>
[[nodiscard]] std::vector<std::uint64_t> SystemIds(After<TSystems...>*) {
    return {SystemId<TSystems>()...};
}

template<typename TSystem>
[[nodiscard]] SystemSchedule MakeSchedule() {
    if constexpr (requires { TSystem::PHASE; }) {
        static_assert(TSystem::PHASE == ESystemPhase::UPDATE, "Only the Update phase is currently supported");
    }
    SystemSchedule schedule{.Id = SystemId<TSystem>()};
    if constexpr (requires { typename TSystem::Before; }) {
        schedule.Before = SystemIds(static_cast<typename TSystem::Before*>(nullptr));
    }
    if constexpr (requires { typename TSystem::After; }) {
        schedule.After = SystemIds(static_cast<typename TSystem::After*>(nullptr));
    }
    return schedule;
}

// Copyable invocation capability. Saved copies reject access after the run
// or on a foreign thread; returned references must not escape the callback.
template<typename TAccess>
class SystemContext final {
public:
    [[nodiscard]] double DeltaTime() const {
        return GetWorld().GetSystemDeltaTime();
    }

    template<typename... TComponents>
        requires(TAccess::template CAN_READ<TComponents> && ...)
    [[nodiscard]] auto Query() const {
        return GetWorld().template Query<typename TAccess::template Component<TComponents>...>();
    }

    template<typename... TComponents, typename TCallback>
        requires(TAccess::template CAN_READ<TComponents> && ...)
    void ForEach(TCallback&& callback, EQueryEnabledPolicy policy = EQueryEnabledPolicy::SKIP_DISABLED) const {
        GetWorld().template ForEach<typename TAccess::template Component<TComponents>...>(
                std::forward<TCallback>(callback),
                policy);
    }

    [[nodiscard]] EntityRef GetEntity(EntityReference entity) const {
        return GetWorld().GetEntityRef(entity);
    }

    template<typename T>
        requires TAccess::template
    CAN_READ<T> [[nodiscard]] typename TAccess::template Component<T>& Get(EntityReference entity) const {
        return GetEntity(entity).template GetComponent<typename TAccess::template Component<T>>();
    }

private:
    explicit SystemContext(World& world)
        : m_world(&world)
        , m_lifetime(world.LifetimeToken_)
        , m_invocation(world.CaptureQueryInvocation()) {
    }

    [[nodiscard]] World& GetWorld() const {
        if (m_lifetime.expired()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "SystemContext World lifetime has ended");
        }
        m_world->ValidateQueryInvocation(m_invocation);
        return *m_world;
    }

private:
    World* m_world;
    std::weak_ptr<bool> m_lifetime;
    std::uint64_t m_invocation;
    friend class NScene::Scene;
};

} // namespace NEcs::NSystem
