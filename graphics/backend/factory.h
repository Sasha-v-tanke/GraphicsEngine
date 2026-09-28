#pragma once

#include <memory>

#include <graphics/graphics_config.h>

namespace NGraphics::NBackend {

class IGraphicsBackend;

using GraphicsBackendFactory = std::unique_ptr<IGraphicsBackend> (*)(const GraphicsConfig& config);

[[nodiscard]] std::unique_ptr<IGraphicsBackend> CreateGraphicsBackend(const GraphicsConfig& config);

void SetGraphicsBackendFactoryForTests(GraphicsBackendFactory factory) noexcept;

} // namespace NGraphics::NBackend
