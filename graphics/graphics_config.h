#pragma once

#include <graphics/graphics_capabilities.h>

namespace NGraphics {

enum class EGraphicsBackend {
    DEFAULT,
};

struct GraphicsConfig {
    EGraphicsBackend Backend = EGraphicsBackend::DEFAULT;
    RequiredGraphicsCapabilities RequiredCapabilities;
};

} // namespace NGraphics
