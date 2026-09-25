#pragma once

namespace NWindow {

// Logical or framebuffer size in pixels, depending on the API returning it.
struct WindowSize {
    int Width = 0;
    int Height = 0;

    bool operator==(const WindowSize&) const = default;
};

} // namespace NWindow
