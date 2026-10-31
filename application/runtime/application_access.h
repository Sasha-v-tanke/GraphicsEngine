#pragma once

#include <application/application.h>
#include <application/runtime/frame_loop.h>

namespace NApplication::NRuntime {

// Internal composition seam for runtime integration and tests.
class ApplicationAccess final {
public:
    [[nodiscard]] static MainExecutor& GetMainExecutor(Application& application) noexcept {
        return application.m_frameLoop->GetMainExecutor();
    }
};

} // namespace NApplication::NRuntime
