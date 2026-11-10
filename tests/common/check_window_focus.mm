#include <cerrno>
#include <chrono>
#include <crt_externs.h>
#include <cstdio>
#include <spawn.h>
#include <thread>
#include <unistd.h>

#import <AppKit/AppKit.h>
#include <sys/wait.h>

namespace {

pid_t GetFrontmostProcess() {
    @autoreleasepool {
        return NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    }
}

} // namespace

// A separate observer avoids changing NSApplication setup in the process under test.
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("Usage: GraphicsEngineCheckWindowFocus command [args...]\n", stderr);
        return 2;
    }

    const pid_t initial = GetFrontmostProcess();
    if (initial <= 0) {
        std::fputs("No frontmost application; run in a macOS GUI session\n", stderr);
        return 77;
    }

    pid_t child = 0;
    const int error = posix_spawnp(&child, argv[1], nullptr, nullptr, argv + 1, *_NSGetEnviron());
    if (error != 0) {
        std::fprintf(stderr, "Cannot launch focus check command: %d\n", error);
        return 2;
    }

    bool changed = false;
    int status = 0;
    for (;;) {
        const pid_t current = GetFrontmostProcess();
        if (current != initial && !changed) {
            std::fprintf(stderr, "Frontmost process changed: %d -> %d\n", initial, current);
            changed = true;
        }

        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            break;
        }
        if (result < 0 && errno != EINTR) {
            std::perror("waitpid");
            return 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    if (changed || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 1;
    }
    std::printf("Frontmost process remained %d throughout the command\n", initial);
    return 0;
}
