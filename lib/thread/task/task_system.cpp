#include "task_system.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <system_error>
#ifndef NDEBUG
    #include <unordered_set>
#endif
#include <utility>

#include <lib/common/error/exception.h>

namespace NCommon {

namespace {

struct WorkerThreadContext {
    TaskSystem* System = nullptr;
    WorkerIndex Index{0};
};

thread_local WorkerThreadContext CurrentWorker;

[[nodiscard]] ErrorInfo MakeFailureError(const std::exception_ptr& error) noexcept {
    try {
        if (error == nullptr) {
            return {
                    .Code = make_error_code(EError::UNKNOWN),
                    .Message = "Unknown task failure",
            };
        }

        try {
            std::rethrow_exception(error);
        } catch (const NCommon::Exception& exception) {
            return {
                    .Code = exception.code(),
                    .Message = exception.GetMessage(),
            };
        } catch (const std::system_error& exception) {
            return {
                    .Code = exception.code(),
                    .Message = exception.what(),
            };
        } catch (const std::exception& exception) {
            return {
                    .Code = make_error_code(EError::UNKNOWN),
                    .Message = exception.what(),
            };
        } catch (...) {
            return {
                    .Code = make_error_code(EError::UNKNOWN),
                    .Message = "Unknown non-standard exception",
            };
        }
    } catch (...) {
        return {
                .Code = make_error_code(EError::OUT_OF_MEMORY),
                .Message = {},
        };
    }
}

} // namespace

struct TaskSystem::OwnerToken {};

struct TaskHandle::State {
    mutable std::mutex Mutex;
    std::condition_variable Condition;
    std::shared_ptr<const TaskSystem::OwnerToken> Owner;
    std::uint64_t Id = 0;
    TaskSystem::Task Task;
};

bool TaskHandle::IsValid() const noexcept {
    return m_id != 0 && m_state != nullptr;
}

TaskHandle TaskContext::Spawn(TaskFunction function) {
    return m_taskSystem->Submit(std::move(function));
}

TaskHandle TaskContext::Spawn(TaskFunction function, std::span<const TaskHandle> dependencies) {
    return m_taskSystem->Submit(std::move(function), dependencies);
}

TaskSystem::TaskSystem(std::size_t workerCount)
    : m_ownerToken(std::make_shared<OwnerToken>())
    , m_workerCount(workerCount == 0 ? 1 : workerCount) {
    m_workerReadyQueues.reserve(m_workerCount);
    m_workers.reserve(m_workerCount);

    try {
        for (std::size_t index = 0; index < m_workerCount; ++index) {
            m_workerReadyQueues.push_back(std::make_unique<ReadyQueue>());
        }

        for (std::size_t index = 0; index < m_workerCount; ++index) {
            m_workers.emplace_back([this, workerIndex = WorkerIndex{index}] { WorkerLoop(workerIndex); });
        }
    } catch (...) {
        StopWorkers();
        throw;
    }
}

TaskSystem::~TaskSystem() {
    StopWorkers();
}

TaskHandle TaskSystem::SubmitImpl(TaskFunction function, std::span<const TaskHandle> dependencies) {
    if (!function) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Task function is empty");
    }

    std::lock_guard lock{m_mutex};
    DrainRetiredTasksLocked();

    if (m_stopping.load(std::memory_order_acquire)) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "TaskSystem is stopping");
    }

    Task task;
    task.Function = std::move(function);
    task.Dependencies.reserve(dependencies.size());

    for (const TaskHandle& dependency: dependencies) {
        task.Dependencies.push_back(dependency.GetId());
    }

    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        const TaskHandle& dependency = dependencies[index];

        static_cast<void>(GetTaskLocked(dependency));

        if (std::ranges::find(dependencies.begin(),
                              dependencies.begin() + static_cast<std::ptrdiff_t>(index),
                              dependency) != dependencies.begin() + static_cast<std::ptrdiff_t>(index)) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT,
                                  "Task {} is specified more than once as a dependency",
                                  dependency.GetId());
        }
    }

    auto state = std::make_shared<TaskHandle::State>();
    const std::uint64_t id = m_nextTaskId++;
    state->Owner = m_ownerToken;
    state->Id = id;

    std::vector<std::uint64_t> linkedDependencies;
    bool outstandingCommitted = false;

    try {
        linkedDependencies.reserve(dependencies.size());

        for (const TaskHandle& dependency: dependencies) {
            static_cast<void>(GetTaskLocked(dependency));
            std::lock_guard dependencyLock{dependency.m_state->Mutex};
            Task& dependencyTask = dependency.m_state->Task;

            if (!IsTerminalLocked(dependencyTask)) {
                dependencyTask.Dependents.push_back(id);
                linkedDependencies.push_back(dependency.GetId());
                task.RemainingDependencies.fetch_add(1, std::memory_order_relaxed);
            } else if (!IsSuccessfulLocked(dependencyTask)) {
                task.Status = ETaskStatus::CANCELLED;
            }
        }

        if (task.Status != ETaskStatus::CANCELLED) {
            task.Status = task.RemainingDependencies.load(std::memory_order_relaxed) == 0 ? ETaskStatus::READY
                                                                                          : ETaskStatus::WAITING;
        }

        const TaskHandle handle{id, state};
        state->Task = std::move(task);
        m_activeTasks.emplace(id, state);

        Task& storedTask = state->Task;

        if (storedTask.Status == ETaskStatus::READY) {
            m_outstandingTasks.fetch_add(1, std::memory_order_release);
            outstandingCommitted = true;
            MakeReadyLocked(id, storedTask);
        } else if (storedTask.Status == ETaskStatus::WAITING) {
            m_outstandingTasks.fetch_add(1, std::memory_order_release);
            outstandingCommitted = true;
        } else if (storedTask.Status == ETaskStatus::CANCELLED) {
            ReleaseExecutionPayloadLocked(storedTask);
            m_activeTasks.erase(id);
        }

#ifndef NDEBUG
        ValidateDagLocked();
#endif
        return handle;
    } catch (...) {
        for (const std::uint64_t dependencyId: linkedDependencies) {
            const auto dependencyIt = m_activeTasks.find(dependencyId);

            if (dependencyIt == m_activeTasks.end()) {
                continue;
            }

            std::vector<std::uint64_t>& dependents = dependencyIt->second->Task.Dependents;
            const auto dependentIt = std::ranges::find(dependents, id);

            if (dependentIt != dependents.end()) {
                dependents.erase(dependentIt);
            }
        }

        m_activeTasks.erase(id);

        if (outstandingCommitted) {
            ReleaseOutstandingTask();
        }

        if (m_nextTaskId == id + 1) {
            --m_nextTaskId;
        }

        throw;
    }
}

ETaskStatus TaskSystem::GetStatus(const TaskHandle& task) const {
    static_cast<void>(GetTaskLocked(task));

    std::lock_guard taskLock{task.m_state->Mutex};
    return task.m_state->Task.Status;
}

std::optional<ErrorInfo> TaskSystem::GetError(const TaskHandle& task) const {
    static_cast<void>(GetTaskLocked(task));

    std::lock_guard taskLock{task.m_state->Mutex};
    return task.m_state->Task.Error;
}

void TaskSystem::Cancel(const TaskHandle& task) {
    std::lock_guard lock{m_mutex};

    static_cast<void>(GetTaskLocked(task));
    std::lock_guard taskLock{task.m_state->Mutex};
    Task& storedTask = task.m_state->Task;

    if (IsTerminalLocked(storedTask)) {
        return;
    }

    if (storedTask.Status == ETaskStatus::RUNNING) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Running task cannot be cancelled");
    }

    storedTask.Status = ETaskStatus::CANCELLED;
    PropagateCancellationLocked(storedTask);
    ReleaseExecutionPayloadLocked(storedTask);
    m_activeTasks.erase(task.GetId());
    task.m_state->Condition.notify_all();
    ReleaseOutstandingTask();
}

void TaskSystem::Wait(const TaskHandle& task) {
    if (CurrentWorker.System == this) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE,
                              "TaskSystem::Wait cannot be called from a worker thread of the same TaskSystem");
    }

    static_cast<void>(GetTaskLocked(task));

    std::unique_lock taskLock{task.m_state->Mutex};
    task.m_state->Condition.wait(taskLock, [&task] { return IsTerminalLocked(task.m_state->Task); });
}

void TaskSystem::WaitIdle() {
    if (CurrentWorker.System == this) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE,
                              "TaskSystem::WaitIdle cannot be called from a worker thread of the same TaskSystem");
    }

    while (true) {
        const std::size_t outstanding = m_outstandingTasks.load(std::memory_order_acquire);

        if (outstanding == 0) {
            break;
        }

        m_outstandingTasks.wait(outstanding, std::memory_order_acquire);
    }

    std::lock_guard lock{m_mutex};
    DrainRetiredTasksLocked();
}

TaskSystem::Task& TaskSystem::GetTaskLocked(const TaskHandle& task) {
    if (!task.IsValid()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Invalid task handle");
    }

    const auto& state = task.m_state;

    if (state->Owner != m_ownerToken || state->Id != task.GetId()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Task {} belongs to another TaskSystem", task.GetId());
    }

    return state->Task;
}

const TaskSystem::Task& TaskSystem::GetTaskLocked(const TaskHandle& task) const {
    if (!task.IsValid()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Invalid task handle");
    }

    const auto& state = task.m_state;

    if (state->Owner != m_ownerToken || state->Id != task.GetId()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Task {} belongs to another TaskSystem", task.GetId());
    }

    return state->Task;
}

bool TaskSystem::IsTerminalLocked(const Task& task) noexcept {
    return task.Status == ETaskStatus::COMPLETED || task.Status == ETaskStatus::FAILED ||
           task.Status == ETaskStatus::CANCELLED;
}

bool TaskSystem::IsSuccessfulLocked(const Task& task) noexcept {
    return task.Status == ETaskStatus::COMPLETED;
}

void TaskSystem::MakeReadyLocked(std::uint64_t taskId, Task& task) {
    task.Status = ETaskStatus::READY;
    PublishReadyTask(m_activeTasks.at(taskId));
}

void TaskSystem::PublishReadyTask(std::shared_ptr<TaskHandle::State> state) {
    if (CurrentWorker.System == this) {
        ReadyQueue& queue = *m_workerReadyQueues[CurrentWorker.Index.GetValue()];
        {
            std::lock_guard queueLock{queue.Mutex};
            queue.Tasks.push_front(std::move(state));
        }
    } else {
        std::lock_guard queueLock{m_injectedReadyQueue.Mutex};
        m_injectedReadyQueue.Tasks.push_back(std::move(state));
    }

    m_readyTaskCount.fetch_add(1, std::memory_order_release);
    m_readyWakeups.release();
}

std::shared_ptr<TaskHandle::State> TaskSystem::TryPopReadyTask(WorkerIndex workerIndex) {
    if (auto state = TryPopLocalReadyTask(workerIndex)) {
        return state;
    }

    if (auto state = TryPopInjectedReadyTask()) {
        return state;
    }

    return TryStealReadyTask(workerIndex);
}

std::shared_ptr<TaskHandle::State> TaskSystem::TryPopLocalReadyTask(WorkerIndex workerIndex) {
    ReadyQueue& queue = *m_workerReadyQueues[workerIndex.GetValue()];
    std::lock_guard queueLock{queue.Mutex};

    if (queue.Tasks.empty()) {
        return {};
    }

    std::shared_ptr<TaskHandle::State> state = std::move(queue.Tasks.front());
    queue.Tasks.pop_front();
    m_readyTaskCount.fetch_sub(1, std::memory_order_acq_rel);
    return state;
}

std::shared_ptr<TaskHandle::State> TaskSystem::TryPopInjectedReadyTask() {
    std::lock_guard queueLock{m_injectedReadyQueue.Mutex};

    if (m_injectedReadyQueue.Tasks.empty()) {
        return {};
    }

    std::shared_ptr<TaskHandle::State> state = std::move(m_injectedReadyQueue.Tasks.front());
    m_injectedReadyQueue.Tasks.pop_front();
    m_readyTaskCount.fetch_sub(1, std::memory_order_acq_rel);
    return state;
}

std::shared_ptr<TaskHandle::State> TaskSystem::TryStealReadyTask(WorkerIndex workerIndex) {
    for (std::size_t offset = 1; offset < m_workerCount; ++offset) {
        const std::size_t victimIndex = (workerIndex.GetValue() + offset) % m_workerCount;
        ReadyQueue& queue = *m_workerReadyQueues[victimIndex];
        std::lock_guard queueLock{queue.Mutex};

        if (queue.Tasks.empty()) {
            continue;
        }

        std::shared_ptr<TaskHandle::State> state = std::move(queue.Tasks.back());
        queue.Tasks.pop_back();
        m_readyTaskCount.fetch_sub(1, std::memory_order_acq_rel);
        return state;
    }

    return {};
}

bool TaskSystem::TryClaimReadyTask(const std::shared_ptr<TaskHandle::State>& state,
                                   TaskHandle& task,
                                   TaskFunction& function) {
    std::lock_guard taskLock{state->Mutex};

    if (state->Task.Status != ETaskStatus::READY) {
        state->Condition.notify_all();
        return false;
    }

    state->Task.Status = ETaskStatus::RUNNING;
    task = TaskHandle{state->Id, state};
    function = std::move(state->Task.Function);
    state->Condition.notify_all();
    return true;
}

void TaskSystem::RetireTask(std::uint64_t taskId) {
    std::lock_guard lock{m_retiredMutex};
    m_retiredTasks.push_back(taskId);
}

void TaskSystem::DrainRetiredTasksLocked() {
    std::deque<std::uint64_t> retiredTasks;

    {
        std::lock_guard lock{m_retiredMutex};
        retiredTasks.swap(m_retiredTasks);
    }

    for (const std::uint64_t taskId: retiredTasks) {
        m_activeTasks.erase(taskId);
    }
}

void TaskSystem::ReleaseOutstandingTask() noexcept {
    const std::size_t previousOutstanding = m_outstandingTasks.fetch_sub(1, std::memory_order_acq_rel);

    if (previousOutstanding == 1) {
        m_outstandingTasks.notify_all();
    }
}

void TaskSystem::CompleteState(const std::shared_ptr<TaskHandle::State>& state,
                               ETaskStatus status,
                               std::optional<ErrorInfo> error) {
    std::vector<std::uint64_t> dependents;

    {
        std::lock_guard taskLock{state->Mutex};
        state->Task.Status = status;
        state->Task.Error = std::move(error);
        dependents = state->Task.Dependents;

        if (status == ETaskStatus::COMPLETED && dependents.empty()) {
            ReleaseExecutionPayloadLocked(state->Task);
        }
    }

    if (status == ETaskStatus::COMPLETED && !dependents.empty()) {
        std::lock_guard lock{m_mutex};

        for (const std::uint64_t dependent: dependents) {
            const auto dependentIt = m_activeTasks.find(dependent);

            if (dependentIt == m_activeTasks.end()) {
                continue;
            }

            auto& dependentState = dependentIt->second;
            std::lock_guard dependentLock{dependentState->Mutex};
            Task& dependentTask = dependentState->Task;

            if (dependentTask.Status != ETaskStatus::WAITING) {
                continue;
            }

            const std::size_t previousRemaining =
                    dependentTask.RemainingDependencies.fetch_sub(1, std::memory_order_acq_rel);

            if (previousRemaining == 0) {
                FailDagInvariantLocked("Task dependency counter underflow while completing prerequisite");
            }

            if (previousRemaining == 1) {
                dependentTask.Status = ETaskStatus::READY;
                PublishReadyTask(dependentState);
            }
        }

        {
            std::lock_guard taskLock{state->Mutex};
            ReleaseExecutionPayloadLocked(state->Task);
        }
    } else if (status != ETaskStatus::COMPLETED) {
        std::lock_guard lock{m_mutex};
        std::lock_guard taskLock{state->Mutex};
        PropagateCancellationLocked(state->Task);
        ReleaseExecutionPayloadLocked(state->Task);
    }

    state->Condition.notify_all();
    RetireTask(state->Id);
    ReleaseOutstandingTask();
}

void TaskSystem::PropagateCancellationLocked(Task& task) {
    for (const std::uint64_t dependent: task.Dependents) {
        const auto dependentIt = m_activeTasks.find(dependent);

        if (dependentIt == m_activeTasks.end()) {
            continue;
        }

        auto dependentState = dependentIt->second;
        std::lock_guard dependentLock{dependentState->Mutex};
        Task& dependentTask = dependentState->Task;

        if (IsTerminalLocked(dependentTask) || dependentTask.Status == ETaskStatus::RUNNING) {
            continue;
        }

        dependentTask.Status = ETaskStatus::CANCELLED;
        PropagateCancellationLocked(dependentTask);
        ReleaseExecutionPayloadLocked(dependentTask);
        m_activeTasks.erase(dependent);
        dependentState->Condition.notify_all();
        ReleaseOutstandingTask();
    }
}

void TaskSystem::CancelPendingTasksLocked() noexcept {
    for (auto taskIt = m_activeTasks.begin(); taskIt != m_activeTasks.end();) {
        auto state = taskIt->second;
        std::lock_guard taskLock{state->Mutex};
        Task& task = state->Task;

        if (task.Status != ETaskStatus::WAITING && task.Status != ETaskStatus::READY) {
            ++taskIt;
            continue;
        }

        task.Status = ETaskStatus::CANCELLED;
        ReleaseExecutionPayloadLocked(task);
        taskIt = m_activeTasks.erase(taskIt);
        state->Condition.notify_all();
        ReleaseOutstandingTask();
    }
}

void TaskSystem::ReleaseExecutionPayloadLocked(Task& task) {
    task.Function = {};
    task.Dependencies.clear();
    task.Dependents.clear();
    task.RemainingDependencies.store(0, std::memory_order_relaxed);
}

[[noreturn]] void TaskSystem::FailDagInvariantLocked(const char* message) noexcept {
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
    std::terminate();
}

#ifndef NDEBUG
void TaskSystem::ValidateDagLocked() const {
    for (const auto& [taskId, state]: m_activeTasks) {
        std::lock_guard taskLock{state->Mutex};
        const Task& task = state->Task;
        std::unordered_set<std::uint64_t> uniqueDependencies;
        std::size_t unfinishedDependencies = 0;

        for (const std::uint64_t dependencyId: task.Dependencies) {
            if (!uniqueDependencies.insert(dependencyId).second) {
                FailDagInvariantLocked("Task has duplicate dependency");
            }

            const auto dependencyIt = m_activeTasks.find(dependencyId);

            if (dependencyIt == m_activeTasks.end()) {
                continue;
            }

            std::lock_guard dependencyLock{dependencyIt->second->Mutex};

            const std::vector<std::uint64_t>& dependents = dependencyIt->second->Task.Dependents;
            const bool hasBacklink = std::ranges::find(dependents, taskId) != dependents.end();

            if (!hasBacklink && IsSuccessfulLocked(dependencyIt->second->Task)) {
                continue;
            }

            if (!hasBacklink) {
                FailDagInvariantLocked("Task dependency does not link back to dependent");
            }

            if (task.Status == ETaskStatus::WAITING) {
                ++unfinishedDependencies;
            }
        }

        if (task.Status == ETaskStatus::WAITING &&
            task.RemainingDependencies.load(std::memory_order_relaxed) != unfinishedDependencies) {
            FailDagInvariantLocked("Task remaining dependency counter is inconsistent");
        }

        std::unordered_set<std::uint64_t> uniqueDependents;

        for (const std::uint64_t dependentId: task.Dependents) {
            if (!uniqueDependents.insert(dependentId).second) {
                FailDagInvariantLocked("Task has duplicate dependent");
            }

            const auto dependentIt = m_activeTasks.find(dependentId);

            if (dependentIt == m_activeTasks.end()) {
                continue;
            }

            std::lock_guard dependentLock{dependentIt->second->Mutex};
            const std::vector<std::uint64_t>& dependencies = dependentIt->second->Task.Dependencies;

            if (std::ranges::find(dependencies, taskId) == dependencies.end()) {
                FailDagInvariantLocked("Task dependent does not link back to dependency");
            }
        }
    }
}
#endif

void TaskSystem::StopWorkers() noexcept {
    {
        std::lock_guard lock{m_mutex};
        m_stopping.store(true, std::memory_order_release);
        CancelPendingTasksLocked();
    }

    m_readyWakeups.release(static_cast<std::ptrdiff_t>(m_workerCount));
    m_outstandingTasks.notify_all();

    for (std::thread& worker: m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void TaskSystem::WorkerLoop(WorkerIndex workerIndex) noexcept {
    CurrentWorker.System = this;
    CurrentWorker.Index = workerIndex;

    while (true) {
        std::shared_ptr<TaskHandle::State> state;
        TaskHandle task;
        TaskFunction function;

        try {
            while (true) {
                if (m_stopping.load(std::memory_order_acquire) &&
                    m_readyTaskCount.load(std::memory_order_acquire) == 0) {
                    return;
                }

                m_readyWakeups.acquire();

                state = TryPopReadyTask(workerIndex);

                if (state && TryClaimReadyTask(state, task, function)) {
                    break;
                }

                if (m_stopping.load(std::memory_order_acquire) &&
                    m_readyTaskCount.load(std::memory_order_acquire) == 0) {
                    return;
                }
            }

            std::optional<ErrorInfo> error;

            try {
                TaskContext context{*this, task, workerIndex};
                function(context);
            } catch (...) {
                error = MakeFailureError(std::current_exception());
            }

            function = {};
            CompleteState(state, error.has_value() ? ETaskStatus::FAILED : ETaskStatus::COMPLETED, std::move(error));
        } catch (...) {
            if (state) {
                CompleteState(state, ETaskStatus::FAILED, MakeFailureError(std::current_exception()));
            } else {
                m_outstandingTasks.notify_all();
            }
        }
    }
}

} // namespace NCommon
