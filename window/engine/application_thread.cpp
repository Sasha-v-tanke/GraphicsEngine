#include "application_thread.h"

#include <cstddef>
#include <exception>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NWindow::NEngine {

namespace {

struct ApplicationThreadState {
    std::optional<std::thread::id> ThreadId;
    std::size_t RuntimeCount = 0;
};

std::mutex& GetMutex() {
    static std::mutex mutex;
    return mutex;
}

ApplicationThreadState& GetState() {
    static ApplicationThreadState state;
    return state;
}

} // namespace

void RegisterApplicationThread() {
    std::lock_guard lock{GetMutex()};

    ApplicationThreadState& state = GetState();
    const std::thread::id threadId = std::this_thread::get_id();

    if (state.ThreadId.has_value() && state.ThreadId != threadId) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Window runtime already belongs to another application thread");
    }

    state.ThreadId = threadId;
    ++state.RuntimeCount;
}

void UnregisterApplicationThread() noexcept {
    std::lock_guard lock{GetMutex()};

    ApplicationThreadState& state = GetState();

    if (!state.ThreadId.has_value() || state.ThreadId != std::this_thread::get_id()) {
        std::terminate();
    }

    --state.RuntimeCount;

    if (state.RuntimeCount == 0) {
        state.ThreadId.reset();
    }
}

void ValidateApplicationThread(std::string_view operation) {
    std::lock_guard lock{GetMutex()};

    const ApplicationThreadState& state = GetState();

    if (!state.ThreadId.has_value()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "{} requires an active WindowRuntime on the application thread",
                              operation);
    }

    if (state.ThreadId != std::this_thread::get_id()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "{} must run on the registered application thread",
                              operation);
    }
}

} // namespace NWindow::NEngine
