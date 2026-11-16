#include "entity_ref.h"

namespace NEcs {

EntityRef::EntityRef(World& world, EntityReference reference)
    : m_world(&world)
    , m_reference(reference)
    , m_lifetime(world.LifetimeToken_) {
}

World& EntityRef::GetWorld() const {
    if (m_world == nullptr || m_lifetime.expired()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "EntityRef World lifetime has ended");
    }
    m_world->ValidateEntityRefContext(*this);
    return *m_world;
}

void EntityRef::Destroy() {
    GetWorld().QueueDestroyEntity(m_reference);
}

bool EntityRef::IsPending() const {
    if (m_world == nullptr || m_lifetime.expired()) {
        return false;
    }
    return GetWorld().GetStructuralState(m_reference) == World::EStructuralState::PENDING_CREATE;
}

bool EntityRef::IsAlive() const {
    if (m_world == nullptr || m_lifetime.expired()) {
        return false;
    }
    const auto state = GetWorld().GetStructuralState(m_reference);
    return state == World::EStructuralState::ALIVE || state == World::EStructuralState::PENDING_DESTROY;
}

EntityReference EntityRef::GetReference() const noexcept {
    return m_reference;
}

void EntityRef::ThrowMissingComponent() const {
    if (IsPending()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_READY, "EntityRef creation has not been committed");
    }
    GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_FOUND, "EntityRef component does not exist");
}

} // namespace NEcs
