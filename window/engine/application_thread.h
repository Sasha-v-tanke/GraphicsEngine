#pragma once

#include <string_view>

namespace NWindow::NEngine {

void RegisterApplicationThread();

void UnregisterApplicationThread() noexcept;

void ValidateApplicationThread(std::string_view operation);

} // namespace NWindow::NEngine
