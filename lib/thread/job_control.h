#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>

#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NCommon::NJobInternal {

struct JobSignal {
    std::atomic<std::uint64_t> Version{0};
    std::atomic_bool Stopping{false};
    void Wake() noexcept {
        Version.fetch_add(1);
        Version.notify_one();
    }
};

class JobControl;

// One registration per suspension. Late notifications cannot wake a later await.
struct JobContinuation {
    std::atomic_bool Ready{false};
    std::weak_ptr<JobControl> Owner;
    void Notify() noexcept;
};

enum class EJobPhase {
    QUEUED,
    RUNNING,
    WAITING,
    RESUME_QUEUED,
    CANCEL_QUEUED,
    STOP_RUNNING,
    FINALIZING,
    READY,
};

// Shared with result handles; never refers to the runtime or its executor.
class JobControl: private NonTransferable {
public:
    virtual ~JobControl() = default;
    std::shared_ptr<JobSignal> Signal;
    std::atomic<EJobPhase> Phase{EJobPhase::QUEUED};
    std::atomic_bool HasConsumer{true};
    std::atomic_bool Observed{false};
    std::atomic_bool Reported{false};
    std::atomic_bool StopDelivered{false};
    std::stop_source StopSource;

    void RequestCancel() noexcept {
        EJobPhase phase = Phase.load();
        while (phase == EJobPhase::QUEUED || phase == EJobPhase::RUNNING || phase == EJobPhase::WAITING ||
               phase == EJobPhase::RESUME_QUEUED) {
            const EJobPhase target = phase == EJobPhase::RUNNING ? EJobPhase::STOP_RUNNING : EJobPhase::CANCEL_QUEUED;
            if (Phase.compare_exchange_weak(phase, target)) {
                if (Signal) {
                    Signal->Wake();
                }
                return;
            }
        }
    }

    [[nodiscard]] bool IsReady() const noexcept {
        return Phase.load() == EJobPhase::READY;
    }

    void Abandon() noexcept {
        HasConsumer.store(false);
        ReportUnobserved();
    }

    // Registration and terminal publication share a lock; callbacks never run under it.
    bool Subscribe(const std::shared_ptr<JobContinuation>& continuation) {
        std::lock_guard lock{m_continuationMutex};
        if (IsReady()) {
            return false;
        }
        m_continuation = continuation;
        return true;
    }

    void SetWaiting(const std::shared_ptr<JobContinuation>& continuation) {
        std::lock_guard lock{m_continuationMutex};
        m_waiting = continuation;
    }

    bool ClaimResume() {
        std::lock_guard lock{m_continuationMutex};
        EJobPhase phase = EJobPhase::WAITING;
        return m_waiting && m_waiting->Ready.load() && Phase.compare_exchange_strong(phase, EJobPhase::RESUME_QUEUED);
    }

    void FailResume() noexcept {
        // Called only by the dispatcher after a failed Submit, before any worker can claim it.
        EJobPhase phase = EJobPhase::RESUME_QUEUED;
        if (Phase.compare_exchange_strong(phase, EJobPhase::FINALIZING)) {
            ReleaseBody();
            SchedulingFailure();
            Publish();
        }
    }

    void Run() noexcept {
        EJobPhase phase = EJobPhase::QUEUED;
        if (!Phase.compare_exchange_strong(phase, EJobPhase::RUNNING)) {
            if (phase != EJobPhase::RESUME_QUEUED || !Phase.compare_exchange_strong(phase, EJobPhase::RUNNING)) {
                return;
            }
        }
        // The scope closes under Engine admission serialization. Recheck after claiming:
        // a worker that raced with Stop must not enter a still-queued user callable.
        if (Signal && Signal->Stopping.load()) {
            Phase.store(EJobPhase::FINALIZING);
            ReleaseBody();
            CancelResult();
            Publish();
            return;
        }
        const bool completed = ExecuteBody();
        if (!completed) {
            phase = EJobPhase::RUNNING;
            if (Phase.compare_exchange_strong(phase, EJobPhase::WAITING)) {
                if (Signal) {
                    Signal->Wake();
                }
                return;
            }
            // Cancellation won while resume was executing. It is now safe to destroy the frame.
            ReleaseBody();
        }
        phase = Phase.exchange(EJobPhase::FINALIZING);
        if (phase == EJobPhase::STOP_RUNNING) {
            CancelResult();
        }
        Publish();
    }

    // Only the cancellation dispatcher calls this, outside runtime/task locks.
    void DispatchCancel() noexcept {
        EJobPhase phase = EJobPhase::CANCEL_QUEUED;
        if (Phase.compare_exchange_strong(phase, EJobPhase::FINALIZING)) {
            ReleaseBody();
            CancelResult();
            Publish();
        } else if (phase == EJobPhase::STOP_RUNNING && !StopDelivered.exchange(true)) {
            StopSource.request_stop();
        }
    }

    void ReportUnobserved() noexcept {
        if (IsReady() && !HasConsumer.load() && !Observed.load() && !Reported.exchange(true)) {
            ReportFailure();
        }
    }

private:
    void Publish() noexcept {
        std::shared_ptr<JobContinuation> continuation;
        {
            std::lock_guard lock{m_continuationMutex};
            Phase.store(EJobPhase::READY);
            continuation = m_continuation.lock();
            m_continuation.reset();
            m_waiting.reset();
        }
        if (continuation) {
            continuation->Notify();
        }
        ReportUnobserved();
        if (Signal) {
            Signal->Wake();
        }
    }
    virtual bool ExecuteBody() noexcept = 0;
    virtual void SchedulingFailure() noexcept = 0;
    virtual void ReleaseBody() noexcept = 0;
    virtual void CancelResult() noexcept = 0;
    virtual void ReportFailure() const noexcept = 0;

private:
    std::mutex m_continuationMutex;
    std::weak_ptr<JobContinuation> m_continuation;
    std::shared_ptr<JobContinuation> m_waiting;
};

inline void JobContinuation::Notify() noexcept {
    Ready.store(true);
    if (const std::shared_ptr<JobControl> owner = Owner.lock(); owner && owner->Signal) {
        owner->Signal->Wake();
    }
}

} // namespace NCommon::NJobInternal
