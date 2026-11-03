#pragma once

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

#include <lib/common/wrapper/non_transferable.h>
#include <lib/thread/job_control.h>
#include <lib/thread/task/task_system.h>

namespace NEngine::NRuntime {

// One scope per Run. The dispatcher delivers cancellation, schedules ready continuations and retires states.
class UserJobScope final: private NCommon::NonTransferable {
public:
    explicit UserJobScope(NCommon::TaskSystem& tasks);
    ~UserJobScope();
    void Add(const std::shared_ptr<NCommon::NJobInternal::JobControl>& job);
    void Remove(const std::shared_ptr<NCommon::NJobInternal::JobControl>& job);
    void CloseAdmission() noexcept {
        m_signal->Stopping.store(true);
        m_signal->Wake();
    }
    void RequestStop() noexcept;
    // Before executor drain, outside Engine locks and outside its workers. The executor must remain alive.
    void Join() noexcept;

private:
    void Dispatch() noexcept;

private:
    NCommon::TaskSystem& m_tasks;
    std::shared_ptr<NCommon::NJobInternal::JobSignal> m_signal;
    std::mutex m_mutex;
    std::list<std::shared_ptr<NCommon::NJobInternal::JobControl>> m_jobs;
    std::atomic_bool m_exit{false};
    std::thread m_dispatcher;
};

} // namespace NEngine::NRuntime
