#pragma once

#include <memory>

#include <window/window_config.h>

namespace NWindow::NInternal {

class IWindowEngine;

using WindowEngineFactory = std::unique_ptr<IWindowEngine> (*)(const WindowConfig& config);

[[nodiscard]] std::unique_ptr<IWindowEngine> CreateWindowEngine(const WindowConfig& config);

void SetWindowEngineFactoryForTests(WindowEngineFactory factory);

} // namespace NWindow::NInternal
