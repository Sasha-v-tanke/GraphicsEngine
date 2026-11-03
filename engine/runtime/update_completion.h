#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>

namespace NEngine::NRuntime {

enum class EUpdateStatus {
    COMPLETED,
    FAILED,
    CANCELLED,
};
struct UpdateOutcome {
    EUpdateStatus Status = EUpdateStatus::COMPLETED;
    std::exception_ptr Error = {};
};

// Semantic completion includes all child systems/producers and their payload retirement.
// Bind before publishing children; Finish is exactly once and must follow quiescence.
class UpdateCompletion final {
public:
    explicit UpdateCompletion(std::function<void(UpdateOutcome)> onFinished);
    [[nodiscard]] bool IsStopRequested() const noexcept;
    void BindCancellation(std::function<void()> cancel);
    void RequestStop() noexcept;
    void Finish(UpdateOutcome outcome = {}) noexcept;
    void Wait() const;

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    bool m_stopping = false;
    bool m_bound = false;
    bool m_finishing = false;
    bool m_finished = false;
    std::function<void()> m_cancel;
    std::function<void(UpdateOutcome)> m_onFinished;
};
} // namespace NEngine::NRuntime
