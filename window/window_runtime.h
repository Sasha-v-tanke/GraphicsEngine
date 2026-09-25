#pragma once

#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NWindow {

// Registers the application thread for window backends with main-thread restrictions.
// Create it before standalone Window objects and destroy it after those windows.
class WindowRuntime final: public NCommon::NonTransferable {
public:
    WindowRuntime();

    ~WindowRuntime();
};

} // namespace NWindow
