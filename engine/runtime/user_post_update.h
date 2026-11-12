#pragma once

#include <atomic>

#include <GraphicsEngine/lib/common/error/exception.h>

namespace NEngine::NRuntime {

// Invocation-local admission, independent of preparing/copying a frame's callback reference.
// The target is borrowed until Engine::Stop drains the frame. Neither cancellation nor destruction invokes it.
class UserPostUpdate final {
public:
    UserPostUpdate(void* target, void (*invoke)(void*))
        : m_target(target)
        , m_invoke(invoke) {
        if (invoke == nullptr) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Post-update callback is empty");
        }
    }

    [[nodiscard]] bool IsWaiting() const noexcept {
        return m_state.load() == EState::WAITING;
    }

    // Linearizes callback start against Cancel. A successful claim is running work that Stop must drain.
    // No Engine mutex is held across user code, including synchronous main-thread handoff.
    [[nodiscard]] bool TryInvoke() {
        EState expected = EState::WAITING;
        if (!m_state.compare_exchange_strong(expected, EState::RUNNING)) {
            return false;
        }
        try {
            m_invoke(m_target);
        } catch (...) {
            m_state.store(EState::FINISHED);
            throw;
        }
        m_state.store(EState::FINISHED);
        return true;
    }

    void Cancel() noexcept {
        EState expected = EState::WAITING;
        (void)m_state.compare_exchange_strong(expected, EState::CANCELLED);
    }

private:
    enum class EState {
        WAITING,
        RUNNING,
        CANCELLED,
        FINISHED
    };
    std::atomic<EState> m_state{EState::WAITING};
    void* m_target;
    void (*m_invoke)(void*);
};

} // namespace NEngine::NRuntime
