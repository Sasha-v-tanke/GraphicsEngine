#include "update_completion.h"

#include <utility>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>
#include <lib/thread/task/task_system.h>

namespace NEngine::NRuntime {
UpdateCompletion::UpdateCompletion(std::function<void(UpdateOutcome)> onFinished)
    : m_onFinished(std::move(onFinished)) {
}
bool UpdateCompletion::IsStopRequested() const noexcept {
    std::lock_guard lock{m_mutex};
    return m_stopping;
}
void UpdateCompletion::BindCancellation(std::function<void()> cancel) {
    std::function<void()> immediate;
    {
        std::lock_guard lock{m_mutex};
        if (m_bound || m_finishing) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Update cancellation is already bound or finished");
        }
        m_bound = true;
        if (m_stopping) {
            immediate = std::move(cancel);
        } else {
            m_cancel = std::move(cancel);
        }
    }
    if (immediate) {
        immediate();
    }
}
void UpdateCompletion::RequestStop() noexcept {
    std::function<void()> cancel;
    {
        std::lock_guard lock{m_mutex};
        m_stopping = true;
        cancel = std::move(m_cancel);
    }
    if (cancel) {
        try {
            cancel();
        } catch (...) {
        }
    }
}
void UpdateCompletion::Finish(UpdateOutcome outcome) noexcept {
    std::function<void(UpdateOutcome)> callback;
    std::function<void()> cancel;
    {
        std::lock_guard lock{m_mutex};
        if (m_finishing) {
            return;
        }
        m_finishing = true;
        callback = std::move(m_onFinished);
        cancel = std::move(m_cancel);
    }
    cancel = {};
    if (callback) {
        try {
            callback(std::move(outcome));
        } catch (...) {
        }
    }
    callback = {};
    {
        std::lock_guard lock{m_mutex};
        m_finished = true;
    }
    m_changed.notify_all();
}
void UpdateCompletion::Wait() const {
    if (NCommon::TaskSystem::IsCurrentThreadWorker()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "A worker cannot wait for semantic update completion");
    }
    std::unique_lock lock{m_mutex};
    m_changed.wait(lock, [this] { return m_finished; });
}
} // namespace NEngine::NRuntime
