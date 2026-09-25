#pragma once

#include <cstddef>

#include <GraphicsEngine/window/window_config.h>

namespace NApplication {

struct ApplicationConfig {
    NWindow::WindowConfig Window;
    std::size_t MaxActiveFrames = 2;
    std::size_t WorkerCount = 0;
};

} // namespace NApplication
