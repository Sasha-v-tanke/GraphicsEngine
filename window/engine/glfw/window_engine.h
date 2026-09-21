#pragma once

#include <memory>

#include <window/engine/engine.h>
#include <window/window_config.h>

namespace NWindow::NEngine::NGlfw {

[[nodiscard]] std::unique_ptr<IWindowEngine> CreateWindowEngine(const WindowConfig& config);

} // namespace NWindow::NEngine::NGlfw
