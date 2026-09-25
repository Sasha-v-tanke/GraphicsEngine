#pragma once

namespace NWindow {

// Requested window backend. Availability depends on build options and installed backend dependencies.
enum class EWindowType {
    GLFW,
    QT,
    SDL,
};

} // namespace NWindow
