#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
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

enum class EJobPhase {
    QUEUED,
    RUNNING,
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
        while (phase == EJobPhase::QUEUED || phase == EJobPhase::RUNNING) {
            const EJobPhase target = phase == EJobPhase::QUEUED ? EJobPhase::CANCEL_QUEUED : EJobPhase::STOP_RUNNING;
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

    void Run() noexcept {
        EJobPhase phase = EJobPhase::QUEUED;
        if (!Phase.compare_exchange_strong(phase, EJobPhase::RUNNING)) {
            return;
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
        ExecuteBody();
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
        Phase.store(EJobPhase::READY);
        ReportUnobserved();
        if (Signal) {
            Signal->Wake();
        }
    }
    virtual void ExecuteBody() noexcept = 0;
    virtual void ReleaseBody() noexcept = 0;
    virtual void CancelResult() noexcept = 0;
    virtual void ReportFailure() const noexcept = 0;
};

} // namespace NCommon::NJobInternal
