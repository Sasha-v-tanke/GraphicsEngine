#pragma once

#include <cstdint>
#include <memory>

#include <GraphicsEngine/lib/common/type_registry.h>

namespace NTypeRegistryTest {

struct Position {
    int X = 0;
};

enum class EMode : std::int16_t {
    IDLE = -1,
    ACTIVE = 3,
};

void RegisterGameplay(NCommon::TypeRegistry& registry);
void RegisterAssets(NCommon::TypeRegistry& registry);
std::shared_ptr<const NCommon::TypeDescriptor> FindPositionFromOtherModule(const NCommon::TypeRegistry& registry);

} // namespace NTypeRegistryTest
