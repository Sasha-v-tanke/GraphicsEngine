#pragma once

#include <cstdint>

namespace NGraphics {

struct GraphicsCapabilities {
    bool Presentation = false;
    bool TimelineCompletion = false;
    std::uint32_t MaxFramesInFlight = 1;
};

struct RequiredGraphicsCapabilities {
    bool Presentation = true;
    bool TimelineCompletion = false;
    std::uint32_t MaxFramesInFlight = 1;
};

[[nodiscard]] bool SatisfiesRequirements(const GraphicsCapabilities& capabilities,
                                         const RequiredGraphicsCapabilities& requirements) noexcept;

} // namespace NGraphics
