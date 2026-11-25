#pragma once

#include <window/engine/engine.h>
#include <window/window.h>

namespace NWindow::NInternal {

[[nodiscard]] NEngine::IWindowEngine& GetWindowEngine(Window& window);
// Owner-thread lifecycle operation; only after the previous Run has joined all consumers.
void ResetCloseRequest(Window& window);

} // namespace NWindow::NInternal
