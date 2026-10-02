#pragma once

#include <cstdint>
#include <vector>

#include <GraphicsEngine/lib/common/resource_use_record.h>

namespace NGraphics {

struct FrameSubmission {
    std::uint64_t FrameIndex = 0;
    bool RequiresPresentation = true;
    std::vector<NCommon::ResourceUseRecord> ResourceUseRecords;
};

} // namespace NGraphics
