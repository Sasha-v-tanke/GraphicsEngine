#include <cstdio>
#include <cstdlib>

#include <window/engine/test_process.h>

namespace {

class ProcessBootstrap final {
public:
    ProcessBootstrap() {
#if defined(_WIN32)
        const int result = _putenv_s(NWindow::NEngine::NonActivatingTestEnvironment, "1");
#else
        const int result = setenv(NWindow::NEngine::NonActivatingTestEnvironment, "1", 1);
#endif
        if (result != 0) {
            std::fputs("Failed to configure the non-activating test process\n", stderr);
            std::abort();
        }
    }
};

// Compiled directly into every TEST_SUITE, including executables with their own main.
const ProcessBootstrap g_processBootstrap;

} // namespace
