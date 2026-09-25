#pragma once

#include <lib/common/wrapper/non_transferable.h>

namespace NWindow {

class WindowRuntime final: public NCommon::NonTransferable {
public:
    WindowRuntime();

    ~WindowRuntime();
};

} // namespace NWindow
