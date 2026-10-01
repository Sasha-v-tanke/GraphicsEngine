#pragma once

#include <window/engine/engine.h>
#include <window/window.h>

namespace NWindow::NInternal {

[[nodiscard]] NEngine::IWindowEngine& GetWindowEngine(Window& window);

} // namespace NWindow::NInternal
