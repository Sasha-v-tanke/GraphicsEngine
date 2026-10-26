#pragma once

#include <string>
#include <utility>

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

private:
    std::string m_name;
    NEcs::World m_world;
};

} // namespace NScene
