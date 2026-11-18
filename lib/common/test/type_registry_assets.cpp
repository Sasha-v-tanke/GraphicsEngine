#include "type_registry_modules.h"

#include <string>

namespace NTypeRegistryTest {

void RegisterAssets(NCommon::TypeRegistry& registry) {
    registry.Register<EMode>({1001}, "Game.Mode");
    registry.Register<std::string>({1003}, "Core.String");
}

std::shared_ptr<const NCommon::TypeDescriptor> FindPositionFromOtherModule(const NCommon::TypeRegistry& registry) {
    return registry.Find<Position>();
}

} // namespace NTypeRegistryTest
