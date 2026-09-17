#pragma once

namespace NWindow {

struct WindowSize {
    int Width = 0;
    int Height = 0;

    bool operator==(const WindowSize&) const = default;
};

} // namespace NWindow
