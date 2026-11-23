#include "type_registry_modules.h"

#include <GraphicsEngine/lib/common/metadata.h>

namespace NTypeRegistryTest {

void RegisterGameplay(NCommon::TypeRegistry& registry) {
    registry.Register<Position>({1002},
                                "Game.Position",
                                2,
                                NCommon::ETypeKind::COMPONENT,
                                {GRAPHICS_ENGINE_METADATA_FIELD(Position, X, 11, "X")});
}

} // namespace NTypeRegistryTest
