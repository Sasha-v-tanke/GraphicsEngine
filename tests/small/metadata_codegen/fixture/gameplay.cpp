#include "GraphicsEngineMetadataGameplay.gen.h"
#include "fixture_types.h"

int NMetadataFixture::ReadPosition() {
    NCommon::TypeRegistry registry;
    RegisterGameplay(registry);
    registry.Freeze();
    return static_cast<int>(registry.Get<Position>()->GetFields().size());
}
