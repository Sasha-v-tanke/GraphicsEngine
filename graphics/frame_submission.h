#pragma once

#include <cstdint>

namespace NGraphics {

struct FrameSubmission {
    std::uint64_t FrameIndex = 0;
    bool RequiresPresentation = true;
};

} // namespace NGraphics
