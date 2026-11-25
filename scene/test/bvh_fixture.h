#pragma once

#include <GraphicsEngine/scene/static_bvh.h>

namespace NBvhTest {

inline NScene::SpatialRecord
Record(NScene::SceneHandle scene, std::uint32_t index, NScene::SpatialAabb bounds, std::uint64_t world = 115) {
    NScene::SpatialRecord record;
    record.Identity = {scene, {world, {index, 1}}};
    record.Validity = NScene::ESpatialValidity::VALID;
    record.Bounds = NScene::SpatialBounds{bounds, {}};
    record.Mobility = NScene::ESpatialMobility::STATIC;
    record.SourceRevision = 1;
    return record;
}

} // namespace NBvhTest
