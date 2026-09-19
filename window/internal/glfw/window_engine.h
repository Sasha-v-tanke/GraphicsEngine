#pragma once

#include <memory>

#include <window/internal/engine.h>
#include <window/window_config.h>

namespace NWindow::NInternal::NGlfw {

[[nodiscard]] std::unique_ptr<IWindowEngine> CreateWindowEngine(const WindowConfig& config);

} // namespace NWindow::NInternal::NGlfw
