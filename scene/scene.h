#pragma once

#include <memory>
#include <string>
#include <utility>

#include <GraphicsEngine/ecs/entity_ref.h>
#include <GraphicsEngine/ecs/name_component.h>
#include <GraphicsEngine/ecs/system_context.h>
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

    template<typename TSystem, typename... TArgs>
    void AddSystem(TArgs&&... args) {
        using Access = typename TSystem::Access;
        auto schedule = NEcs::NSystem::MakeSchedule<TSystem>();
        const auto structuralOrder = static_cast<std::uint32_t>(schedule.Id);
        auto system = std::make_shared<TSystem>(std::forward<TArgs>(args)...);
        m_world.RegisterTypedSystem(
                static_cast<Access*>(nullptr),
                [system = std::move(system)](NEcs::World& world) {
                    NEcs::NSystem::SystemContext<Access> context{world};
                    system->Update(context);
                },
                std::move(schedule),
                structuralOrder);
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
