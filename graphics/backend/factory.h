#pragma once

#include <memory>

#include <graphics/graphics.h>
#include <graphics/graphics_config.h>

namespace NGraphics::NBackend {

class IGraphicsBackend;

[[nodiscard]] std::unique_ptr<IGraphicsBackend> CreateGraphicsBackend(const GraphicsConfig& config);

[[nodiscard]] Graphics CreateGraphicsForBackend(std::unique_ptr<IGraphicsBackend> backend,
                                                const RequiredGraphicsCapabilities& requiredCapabilities = {});

} // namespace NGraphics::NBackend
