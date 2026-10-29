#include "main_executor.h"

#include <limits>
#include <utility>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NApplication::NRuntime {

namespace {

bool IsTerminal(EMainCommandState state) {
    return state == EMainCommandState::COMPLETED || state == EMainCommandState::FAILED ||
           state == EMainCommandState::SKIPPED;
}

} // namespace

MainCompletion::MainCompletion(std::shared_ptr<State> state)
    : m_state(std::move(state)) {
}

EMainCommandState MainCompletion::GetState() const {
    std::lock_guard lock{m_state->Mutex};
    return m_state->Value;
}

std::exception_ptr MainCompletion::GetFailure() const {
    std::lock_guard lock{m_state->Mutex};
    return m_state->Failure;
}

EMainCommandState MainCompletion::Wait() const {
    std::unique_lock lock{m_state->Mutex};
    if (!IsTerminal(m_state->Value) && std::this_thread::get_id() == m_state->Consumer) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Main consumer cannot wait for queued main work");
    }
    m_state->Changed.wait(lock, [this] { return IsTerminal(m_state->Value); });
    return m_state->Value;
}

MainExecutor::MainExecutor()
    : m_consumer(std::this_thread::get_id()) {
}

MainExecutor::~MainExecutor() {
    Stop();
}

void MainExecutor::ValidateConsumer() const {
    if (std::this_thread::get_id() != m_consumer) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Main executor requires its creating thread");
    }
}

MainExecutor::Generation MainExecutor::Start(Wakeup wakeup) {
    ValidateConsumer();
    std::lock_guard lock{m_mutex};
    if (m_running || m_draining || m_stopping != 0 || m_generation == std::numeric_limits<Generation>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Main executor cannot start in its current state");
    }
    m_wakeup = std::move(wakeup);
    ++m_generation;
    m_running = true;
    return m_generation;
}

void MainExecutor::Finish(Entry& entry, EMainCommandState state, std::exception_ptr failure) {
    // Release arbitrary captures outside both locks, before publishing terminal completion.
    entry.Function = {};
    {
        std::lock_guard lock{entry.Completion->Mutex};
        entry.Completion->Failure = std::move(failure);
        entry.Completion->Value = state;
    }
    entry.Completion->Changed.notify_all();
}

void MainExecutor::Stop() noexcept {
    std::list<Entry> skipped;
    Wakeup wakeup;
    {
        std::lock_guard lock{m_mutex};
        ++m_stopping;
        m_running = false;
        skipped.swap(m_queue);
        std::swap(wakeup, m_wakeup);
    }
    for (Entry& entry: skipped) {
        Finish(entry, EMainCommandState::SKIPPED);
    }
    wakeup = {};
    {
        std::lock_guard lock{m_mutex};
        --m_stopping;
    }
}

MainExecutor::Generation MainExecutor::GetGeneration() const {
    std::lock_guard lock{m_mutex};
    return m_generation;
}

MainCompletion MainExecutor::Post(Generation generation, Command command) {
    if (!command) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Main command is empty");
    }
    Entry entry{std::move(command), std::make_shared<MainCompletion::State>()};
    entry.Completion->Consumer = m_consumer;
    MainCompletion completion{entry.Completion};
    {
        std::lock_guard lock{m_mutex};
        if (m_generation == 0) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Main executor has never started");
        }
        if (m_running && generation == m_generation) {
            m_queue.push_back(std::move(entry));
            // Serialize the native signal with Stop, so it cannot access Window after Stop returns.
            // Only a nonthrowing, non-reentrant platform notifier is allowed here, not a command callback.
            if (m_wakeup) {
                m_wakeup();
            }
            return completion;
        }
    }
    Finish(entry, EMainCommandState::SKIPPED);
    return completion;
}

std::size_t MainExecutor::Drain(std::size_t limit) {
    ValidateConsumer();
    {
        std::lock_guard lock{m_mutex};
        if (m_draining) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Main command dispatch is not reentrant");
        }
        m_draining = true;
    }
    // Everything after this point is nonthrowing except platform mutex failures.
    std::size_t count = 0;
    for (; count < limit; ++count) {
        Entry entry;
        {
            std::lock_guard lock{m_mutex};
            if (!m_running || m_queue.empty()) {
                break;
            }
            entry = std::move(m_queue.front());
            m_queue.pop_front();
            std::lock_guard stateLock{entry.Completion->Mutex};
            entry.Completion->Value = EMainCommandState::RUNNING;
        }
        std::exception_ptr failure;
        try {
            entry.Function();
        } catch (...) {
            failure = std::current_exception();
        }
        Finish(entry, failure ? EMainCommandState::FAILED : EMainCommandState::COMPLETED, failure);
    }
    {
        std::lock_guard lock{m_mutex};
        m_draining = false;
    }
    return count;
}

std::size_t MainExecutor::GetPendingCount() const {
    std::lock_guard lock{m_mutex};
    return m_queue.size();
}

} // namespace NApplication::NRuntime
