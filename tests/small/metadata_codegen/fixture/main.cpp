#include "GraphicsEngineMetadataAssets.gen.h"
#include "GraphicsEngineMetadataGameplay.gen.h"
#include "fixture_types.h"

int main() {
    NCommon::TypeRegistry registry;
    NMetadataFixture::RegisterAssets(registry);
    NMetadataFixture::RegisterGameplay(registry);
    registry.Freeze();
    const auto position = registry.Get<NMetadataFixture::Position>();
    const auto fields = position->GetFields();
    if (registry.GetTypes().size() != 3 || fields.size() != 2 || fields[0].Id.Value != 11 ||
        fields[0].Name != "Horizontal" || fields[0].CppType != typeid(float) || fields[1].Id.Value != 12 ||
        position->GetSchemaVersion() != 2 || position->GetCapabilities().Serializable ||
        registry.Get<NMetadataFixture::Label>()->GetFields()[0].Id.Value != 11 ||
        !registry.Get<NMetadataFixture::EMode>()->GetFields().empty() || NMetadataFixture::ReadPosition() != 2 ||
        NMetadataFixture::ReadMode() != 0x1073) {
        return 1;
    }
    auto value = registry.Make<NMetadataFixture::Label>();
    value.Get<NMetadataFixture::Label>().Text = "generated registration";
    return value.Copy().Get<NMetadataFixture::Label>().Text == "generated registration" ? 0 : 2;
}
