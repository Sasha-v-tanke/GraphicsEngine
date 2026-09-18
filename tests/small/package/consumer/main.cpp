#include <window/window.h>

int main() {
    constexpr NWindow::WindowSize Size{
            .Width = 640,
            .Height = 480,
    };

    return Size.Width == 640 && Size.Height == 480 ? 0 : 1;
}
