#pragma once

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

#include <lib/common/wrapper/non_transferable.h>
#include <lib/thread/job_control.h>

namespace NEngine::NRuntime {

// One scope per Run. The dispatcher only delivers cancellation and retires states.
class UserJobScope final: private NCommon::NonTransferable {
public:
    UserJobScope();
    ~UserJobScope();
    void Add(const std::shared_ptr<NCommon::NJobInternal::JobControl>& job);
    void Remove(const std::shared_ptr<NCommon::NJobInternal::JobControl>& job);
    void CloseAdmission() noexcept {
        m_signal->Stopping.store(true);
        m_signal->Wake();
    }
    void RequestStop() noexcept;
    // After executor drain, outside Engine locks and outside its workers.
    void Join() noexcept;

private:
    void Dispatch() noexcept;

private:
    std::shared_ptr<NCommon::NJobInternal::JobSignal> m_signal;
    std::mutex m_mutex;
    std::list<std::shared_ptr<NCommon::NJobInternal::JobControl>> m_jobs;
    std::atomic_bool m_exit{false};
    std::thread m_dispatcher;
};

} // namespace NEngine::NRuntime
