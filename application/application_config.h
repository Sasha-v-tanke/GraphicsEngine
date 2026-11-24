#pragma once

#include <cstddef>
#include <optional>

#include <GraphicsEngine/application/game_config.h>
#include <GraphicsEngine/graphics/presentation.h>
#include <GraphicsEngine/window/window_config.h>

namespace NApplication {

struct ApplicationConfig {
    // Window configuration used to create the owned application window.
    NWindow::WindowConfig Window;

    // Number of bounded frame slots. Must be greater than zero.
    std::size_t MaxActiveFrames = 2;

    // 0 uses TaskSystem normalization, currently one worker.
    std::size_t WorkerCount = 0;

    // Presentation settings; without Game this selects legacy clear presentation.
    // Application supplies the single MaxActiveFrames value.
    std::optional<NGraphics::PresentationConfig> Presentation = std::nullopt;

    // Opt-in composition. DISABLED preserves the legacy CPU-only/clear presentation selection.
    std::optional<GameConfig> Game = std::nullopt;
};

} // namespace NApplication
