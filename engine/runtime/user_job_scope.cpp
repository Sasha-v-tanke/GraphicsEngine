#include "user_job_scope.h"

#include <utility>

namespace NEngine::NRuntime {

UserJobScope::UserJobScope()
    : m_signal(std::make_shared<NCommon::NJobInternal::JobSignal>())
    , m_dispatcher([this] { Dispatch(); }) {
}

UserJobScope::~UserJobScope() {
    RequestStop();
    Join();
}

void UserJobScope::Add(const std::shared_ptr<NCommon::NJobInternal::JobControl>& job) {
    job->Signal = m_signal;
    std::lock_guard lock{m_mutex};
    m_jobs.push_back(job);
}

void UserJobScope::Remove(const std::shared_ptr<NCommon::NJobInternal::JobControl>& job) {
    std::lock_guard lock{m_mutex};
    m_jobs.remove(job); // The caller retains an owner through rollback.
}

void UserJobScope::RequestStop() noexcept {
    std::lock_guard lock{m_mutex};
    for (const std::shared_ptr<NCommon::NJobInternal::JobControl>& job: m_jobs) {
        job->RequestCancel();
    }
}

void UserJobScope::Join() noexcept {
    m_exit.store(true);
    m_signal->Wake();
    if (m_dispatcher.joinable()) {
        m_dispatcher.join();
    }
}

void UserJobScope::Dispatch() noexcept {
    using NCommon::NJobInternal::EJobPhase;
    for (;;) {
        const std::uint64_t version = m_signal->Version.load();
        std::shared_ptr<NCommon::NJobInternal::JobControl> selected;
        bool retired = false;
        {
            std::lock_guard lock{m_mutex};
            for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
                const EJobPhase phase = (*it)->Phase.load();
                if (phase == EJobPhase::READY) {
                    selected = std::move(*it);
                    m_jobs.erase(it);
                    retired = true;
                    break;
                }
                if (phase == EJobPhase::CANCEL_QUEUED ||
                    (phase == EJobPhase::STOP_RUNNING && !(*it)->StopDelivered.load())) {
                    selected = *it;
                    break;
                }
            }
            if (!selected && m_jobs.empty() && m_exit.load()) {
                return;
            }
        }
        if (selected) {
            if (!retired) {
                selected->DispatchCancel();
            }
            continue; // Last owners and user values are destroyed outside the scope lock.
        }
        m_signal->Version.wait(version);
    }
}

} // namespace NEngine::NRuntime
