#pragma once

#include <cstddef>

#include <GraphicsEngine/window/window_config.h>

namespace NApplication {

struct ApplicationConfig {
    // Window configuration used to create the owned application window.
    NWindow::WindowConfig Window;

    // Number of bounded frame slots. Must be greater than zero.
    std::size_t MaxActiveFrames = 2;

    // 0 uses TaskSystem normalization, currently one worker.
    std::size_t WorkerCount = 0;
};

} // namespace NApplication
