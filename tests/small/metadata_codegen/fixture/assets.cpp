#include "GraphicsEngineMetadataAssets.gen.h"
#include "fixture_types.h"

int NMetadataFixture::ReadMode() {
    NCommon::TypeRegistry registry;
    RegisterAssets(registry);
    registry.Freeze();
    return static_cast<int>(registry.Get<EMode>()->GetId().Value);
}
