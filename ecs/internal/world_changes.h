#pragma once

#include <GraphicsEngine/ecs/world.h>

namespace NEcs::NInternal {

// Owner-phase adapter. The tracking clock never substitutes for the World/task barrier.
class WorldChangeAccess final {
public:
    template<typename T>
    static NCommon::ChangeDomainKey Domain(World& world, bool entityRevisions = false) {
        world.ValidateStructuralWriteAllowed();
        return world.GetChangeDomain(typeid(T), entityRevisions);
    }
    static NCommon::WorldEpoch PublishEpoch(World& world);
    static NCommon::ChangeSnapshot CaptureSnapshot(World& world);
};

} // namespace NEcs::NInternal
