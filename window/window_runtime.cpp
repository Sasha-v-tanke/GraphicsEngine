#include "window_runtime.h"

#include <window/engine/application_thread.h>

namespace NWindow {

WindowRuntime::WindowRuntime() {
    NEngine::RegisterApplicationThread();
}

WindowRuntime::~WindowRuntime() {
    NEngine::UnregisterApplicationThread();
}

} // namespace NWindow
