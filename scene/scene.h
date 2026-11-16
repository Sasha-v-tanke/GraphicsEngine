#pragma once

#include <string>
#include <utility>

#include <GraphicsEngine/ecs/entity_ref.h>
#include <GraphicsEngine/ecs/name_component.h>
#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NScene {

class Scene: private NCommon::NonTransferable {
public:
    explicit Scene(std::string name = {})
        : m_name(std::move(name)) {
    }

    virtual ~Scene() = default;

    [[nodiscard]] const std::string& GetName() const noexcept {
        return m_name;
    }

    [[nodiscard]] NEcs::World& GetWorld() noexcept {
        return m_world;
    }

    [[nodiscard]] const NEcs::World& GetWorld() const noexcept {
        return m_world;
    }

    [[nodiscard]] NEcs::EntityRef CreateEntity(std::string name = {}) {
        const NEcs::EntityReference identity =
                name.empty() ? m_world.QueueCreateEntity()
                             : m_world.QueueCreateEntityWithComponent(NEcs::NameComponent{std::move(name)});
        return m_world.GetEntityRef(identity);
    }

    // Offline/initialization convenience. Runtime also commits at the existing safe World boundaries.
    void CommitPending() {
        m_world.CommitPending();
    }

private:
    std::string m_name;
    NEcs::World m_world;
};

} // namespace NScene
