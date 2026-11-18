#include "type_registry_modules.h"

namespace NTypeRegistryTest {

void RegisterGameplay(NCommon::TypeRegistry& registry) {
    registry.Register<Position>({1002}, "Game.Position", 2, NCommon::ETypeKind::COMPONENT);
}

} // namespace NTypeRegistryTest
