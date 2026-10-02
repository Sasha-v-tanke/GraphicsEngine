#pragma once

#include <cstdint>
#include <vector>

#include <GraphicsEngine/resources/resource_use_record.h>

namespace NGraphics {

struct FrameSubmission {
    std::uint64_t FrameIndex = 0;
    bool RequiresPresentation = true;
    std::vector<NResources::ResourceUseRecord> ResourceUseRecords;
};

} // namespace NGraphics
