#pragma once

#include <memory>

#include <window/window_config.h>

namespace NWindow::NInternal {

class IWindowEngine;
class IWindowEventSink;

using WindowEngineFactory = std::unique_ptr<IWindowEngine> (*)(const WindowConfig& config, IWindowEventSink& eventSink);

[[nodiscard]] std::unique_ptr<IWindowEngine> CreateWindowEngine(const WindowConfig& config,
                                                                IWindowEventSink& eventSink);

void SetWindowEngineFactoryForTests(WindowEngineFactory factory);

} // namespace NWindow::NInternal
