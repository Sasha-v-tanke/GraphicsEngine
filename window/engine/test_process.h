#pragma once

#include <cstdlib>
#include <string_view>

namespace NWindow::NEngine {

// Private process contract, set by the test bootstrap before main and inherited by smoke children.
inline constexpr char NonActivatingTestEnvironment[] = "GRAPHICS_ENGINE_INTERNAL_TEST_NON_ACTIVATING";

[[nodiscard]] inline bool IsNonActivatingTestProcess() {
    const char* value = std::getenv(NonActivatingTestEnvironment);
    return value != nullptr && std::string_view{value} == "1";
}

} // namespace NWindow::NEngine
