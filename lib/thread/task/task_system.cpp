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
    std::atomic<TaskHandle::State*> RetiredNext = nullptr;
    std::atomic_bool RetirementQueued = false;
    std::uint64_t Id = 0;
    TaskSystem::Task Task;
};

TaskSystem::TaskRegistry::TaskRegistry()
    : Owner(std::make_shared<OwnerToken>()) {
}

void TaskSystem::TaskRegistry::ValidateHandleOwner(const TaskHandle& task) const {
    if (!task.IsValid()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Invalid task handle");
    }

    const auto& state = task.m_state;

    if (state->Owner != Owner || state->Id != task.GetId()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Task {} belongs to another TaskSystem", task.GetId());
    }
}

std::shared_ptr<TaskHandle::State> TaskSystem::TaskRegistry::AllocateStateLocked() {
    auto state = std::make_shared<TaskHandle::State>();
    state->Owner = Owner;
    state->Id = NextTaskId++;
    return state;
}

void TaskSystem::TaskRegistry::ValidateDependenciesLocked(std::span<const TaskHandle> dependencies) const {
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        const TaskHandle& dependency = dependencies[index];

        ValidateHandleOwner(dependency);

        if (std::ranges::find(dependencies.begin(),
                              dependencies.begin() + static_cast<std::ptrdiff_t>(index),
                              dependency) != dependencies.begin() + static_cast<std::ptrdiff_t>(index)) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT,
                                  "Task {} is specified more than once as a dependency",
                                  dependency.GetId());
        }
    }
}

TaskSystem::LinkedDependencies
TaskSystem::TaskRegistry::LinkDependencyEdgesLocked(std::uint64_t taskId,
                                                    Task& task,
                                                    std::span<const TaskHandle> dependencies) {
    LinkedDependencies result;
    result.LinkedIds.reserve(dependencies.size());

    for (const TaskHandle& dependency: dependencies) {
        ValidateHandleOwner(dependency);
        std::lock_guard dependencyLock{dependency.m_state->Mutex};
        Task& dependencyTask = dependency.m_state->Task;

        if (!TaskLifetime::IsTerminalLocked(dependencyTask)) {
            dependencyTask.Dependents.push_back(taskId);
            result.LinkedIds.push_back(dependency.GetId());
            task.RemainingDependencies.fetch_add(1, std::memory_order_relaxed);
        } else if (!TaskLifetime::IsSuccessfulLocked(dependencyTask)) {
            result.Cancelled = true;
            break;
        }
    }

    if (result.Cancelled) {
        RollbackDependencyEdgesLocked(taskId, result.LinkedIds);
        task.RemainingDependencies.store(0, std::memory_order_relaxed);
    }

    return result;
}

void TaskSystem::TaskRegistry::RollbackDependencyEdgesLocked(std::uint64_t taskId,
                                                             std::span<const std::uint64_t> dependencyIds) {
    for (const std::uint64_t dependencyId: dependencyIds) {
        const auto dependencyIt = ActiveTasks.find(dependencyId);

        if (dependencyIt == ActiveTasks.end()) {
            continue;
        }

        std::lock_guard dependencyLock{dependencyIt->second->Mutex};
        std::vector<std::uint64_t>& dependents = dependencyIt->second->Task.Dependents;
        const auto dependentIt = std::ranges::find(dependents, taskId);

        if (dependentIt != dependents.end()) {
            dependents.erase(dependentIt);
        }
    }
}

void TaskSystem::TaskRegistry::CommitActiveTaskLocked(std::uint64_t taskId, std::shared_ptr<TaskHandle::State> state) {
    ActiveTasks.emplace(taskId, std::move(state));
}

void TaskSystem::TaskRegistry::RollbackAllocationLocked(std::uint64_t taskId) {
    ActiveTasks.erase(taskId);

    if (NextTaskId == taskId + 1) {
        --NextTaskId;
    }
}

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
    : m_workerCount(workerCount == 0 ? 1 : workerCount) {
    m_workers.reserve(m_workerCount);

    try {
        m_readyScheduler.Initialize(m_workerCount);
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
    m_lifetime.DrainRetiredTasksLocked(m_registry);

    if (m_stopping.load(std::memory_order_acquire)) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "TaskSystem is stopping");
    }

    Task task;
    task.Function = std::move(function);
    task.Dependencies.reserve(dependencies.size());
    for (const TaskHandle& dependency: dependencies) {
        task.Dependencies.push_back(dependency.GetId());
    }

    m_registry.ValidateDependenciesLocked(dependencies);

    SubmittedTask submitted;
    submitted.Task = std::move(task);
    submitted.State = m_registry.AllocateStateLocked();
    submitted.Id = submitted.State->Id;

    try {
        LinkedDependencies linked = m_registry.LinkDependencyEdgesLocked(submitted.Id, submitted.Task, dependencies);
        submitted.LinkedDependencies = std::move(linked.LinkedIds);

        if (linked.Cancelled) {
            submitted.Task.Status = ETaskStatus::CANCELLED;
            TaskLifetime::ReleaseExecutionPayloadLocked(submitted.Task);
            submitted.State->Task = std::move(submitted.Task);
            return TaskHandle{submitted.Id, submitted.State};
        }

        submitted.Task.Status = submitted.Task.RemainingDependencies.load(std::memory_order_relaxed) == 0
                                      ? ETaskStatus::READY
                                      : ETaskStatus::WAITING;
        const TaskHandle handle{submitted.Id, submitted.State};
        submitted.State->Task = std::move(submitted.Task);
        m_registry.CommitActiveTaskLocked(submitted.Id, submitted.State);

#ifndef NDEBUG
        ValidateDagLocked();
#endif

        Task& storedTask = submitted.State->Task;
        if (storedTask.Status == ETaskStatus::READY) {
            m_lifetime.OutstandingTasks.fetch_add(1, std::memory_order_release);
            submitted.OutstandingCommitted = true;
            storedTask.Status = ETaskStatus::READY;
            m_readyScheduler.PublishReadyTask(*this, submitted.State);
        } else if (storedTask.Status == ETaskStatus::WAITING) {
            m_lifetime.OutstandingTasks.fetch_add(1, std::memory_order_release);
            submitted.OutstandingCommitted = true;
        }

        return handle;
    } catch (...) {
        m_registry.RollbackDependencyEdgesLocked(submitted.Id, submitted.LinkedDependencies);

        if (submitted.OutstandingCommitted) {
            m_lifetime.ReleaseOutstandingTask();
        }

        m_registry.RollbackAllocationLocked(submitted.Id);

        throw;
    }
}

ETaskStatus TaskSystem::GetStatus(const TaskHandle& task) const {
    ValidateTaskHandleOwner(task);

    std::lock_guard taskLock{task.m_state->Mutex};
    return task.m_state->Task.Status;
}

std::optional<ErrorInfo> TaskSystem::GetError(const TaskHandle& task) const {
    ValidateTaskHandleOwner(task);

    std::lock_guard taskLock{task.m_state->Mutex};
    return task.m_state->Task.Error;
}

void TaskSystem::Cancel(const TaskHandle& task) {
    std::lock_guard lock{m_mutex};

    ValidateTaskHandleOwner(task);
    std::lock_guard taskLock{task.m_state->Mutex};
    Task& storedTask = task.m_state->Task;

    if (TaskLifetime::IsTerminalLocked(storedTask)) {
        return;
    }

    if (storedTask.Status == ETaskStatus::RUNNING) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Running task cannot be cancelled");
    }

    storedTask.Status = ETaskStatus::CANCELLED;
    PropagateCancellationLocked(storedTask);
    TaskLifetime::ReleaseExecutionPayloadLocked(storedTask);
    m_registry.ActiveTasks.erase(task.GetId());
    task.m_state->Condition.notify_all();
    m_lifetime.ReleaseOutstandingTask();
}

void TaskSystem::Wait(const TaskHandle& task) {
    if (CurrentWorker.System == this) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE,
                              "TaskSystem::Wait cannot be called from a worker thread of the same TaskSystem");
    }

    ValidateTaskHandleOwner(task);

    std::unique_lock taskLock{task.m_state->Mutex};
    task.m_state->Condition.wait(taskLock, [&task] { return TaskLifetime::IsTerminalLocked(task.m_state->Task); });
}

void TaskSystem::WaitIdle() {
    if (CurrentWorker.System == this) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE,
                              "TaskSystem::WaitIdle cannot be called from a worker thread of the same TaskSystem");
    }

    while (true) {
        const std::size_t outstanding = m_lifetime.OutstandingTasks.load(std::memory_order_acquire);

        if (outstanding == 0) {
            break;
        }

        m_lifetime.OutstandingTasks.wait(outstanding, std::memory_order_acquire);
    }

    std::lock_guard lock{m_mutex};
    m_lifetime.DrainRetiredTasksLocked(m_registry);
}

void TaskSystem::ValidateTaskHandleOwner(const TaskHandle& task) const {
    m_registry.ValidateHandleOwner(task);
}

void TaskSystem::ReadyScheduler::Initialize(std::size_t workerCount) {
    WorkerQueues.reserve(workerCount);

    for (std::size_t index = 0; index < workerCount; ++index) {
        WorkerQueues.push_back(std::make_unique<ReadyQueue>());
    }
}

void TaskSystem::ReadyScheduler::CompactReadyQueueLocked(ReadyQueue& queue) {
    constexpr std::size_t minConsumedBeforeCompact = 1024;

    if (queue.Head == 0) {
        return;
    }

    if (queue.Head >= queue.Tasks.size()) {
        queue.Tasks.clear();
        queue.Head = 0;
        return;
    }

    if (queue.Head < minConsumedBeforeCompact && queue.Head * 2 < queue.Tasks.size()) {
        return;
    }

    queue.Tasks.erase(queue.Tasks.begin(), queue.Tasks.begin() + static_cast<std::ptrdiff_t>(queue.Head));
    queue.Head = 0;
}

void TaskSystem::ReadyScheduler::PublishReadyTask(TaskSystem& owner, std::shared_ptr<TaskHandle::State> state) {
    if (CurrentWorker.System == &owner) {
        ReadyQueue& queue = *WorkerQueues[CurrentWorker.Index.GetValue()];
        {
            std::lock_guard queueLock{queue.Mutex};
            CompactReadyQueueLocked(queue);
            queue.Tasks.push_back(std::move(state));
        }
    } else {
        std::lock_guard queueLock{InjectedQueue.Mutex};
        CompactReadyQueueLocked(InjectedQueue);
        InjectedQueue.Tasks.push_back(std::move(state));
    }

    ReadyTaskCount.fetch_add(1, std::memory_order_release);
    Wakeups.release();
}

std::shared_ptr<TaskHandle::State> TaskSystem::ReadyScheduler::TryPopReadyTask(WorkerIndex workerIndex) {
    if (auto state = TryPopLocalReadyTask(workerIndex)) {
        return state;
    }

    if (auto state = TryPopInjectedReadyTask()) {
        return state;
    }

    return TryStealReadyTask(workerIndex);
}

bool TaskSystem::ReadyScheduler::HasReadyTasks() const noexcept {
    return ReadyTaskCount.load(std::memory_order_acquire) != 0;
}

void TaskSystem::ReadyScheduler::WakeAll(std::size_t workerCount) {
    Wakeups.release(static_cast<std::ptrdiff_t>(workerCount));
}

std::shared_ptr<TaskHandle::State> TaskSystem::ReadyScheduler::TryPopLocalReadyTask(WorkerIndex workerIndex) {
    ReadyQueue& queue = *WorkerQueues[workerIndex.GetValue()];
    std::lock_guard queueLock{queue.Mutex};

    if (queue.Head >= queue.Tasks.size()) {
        queue.Tasks.clear();
        queue.Head = 0;
        return {};
    }

    std::shared_ptr<TaskHandle::State> state = std::move(queue.Tasks.back());
    queue.Tasks.pop_back();

    if (queue.Head >= queue.Tasks.size()) {
        queue.Tasks.clear();
        queue.Head = 0;
    }

    ReadyTaskCount.fetch_sub(1, std::memory_order_acq_rel);
    return state;
}

std::shared_ptr<TaskHandle::State> TaskSystem::ReadyScheduler::TryPopInjectedReadyTask() {
    std::lock_guard queueLock{InjectedQueue.Mutex};

    if (InjectedQueue.Head >= InjectedQueue.Tasks.size()) {
        InjectedQueue.Tasks.clear();
        InjectedQueue.Head = 0;
        return {};
    }

    std::shared_ptr<TaskHandle::State> state = std::move(InjectedQueue.Tasks[InjectedQueue.Head]);
    ++InjectedQueue.Head;

    CompactReadyQueueLocked(InjectedQueue);

    ReadyTaskCount.fetch_sub(1, std::memory_order_acq_rel);
    return state;
}

std::shared_ptr<TaskHandle::State> TaskSystem::ReadyScheduler::TryStealReadyTask(WorkerIndex workerIndex) {
    for (std::size_t offset = 1; offset < WorkerQueues.size(); ++offset) {
        const std::size_t victimIndex = (workerIndex.GetValue() + offset) % WorkerQueues.size();
        ReadyQueue& queue = *WorkerQueues[victimIndex];
        std::lock_guard queueLock{queue.Mutex};

        if (queue.Head >= queue.Tasks.size()) {
            queue.Tasks.clear();
            queue.Head = 0;
            continue;
        }

        std::shared_ptr<TaskHandle::State> state = std::move(queue.Tasks[queue.Head]);
        ++queue.Head;

        CompactReadyQueueLocked(queue);

        ReadyTaskCount.fetch_sub(1, std::memory_order_acq_rel);
        return state;
    }

    return {};
}

bool TaskSystem::TaskLifetime::TryClaimReadyTask(const std::shared_ptr<TaskHandle::State>& state,
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

bool TaskSystem::TaskLifetime::IsTerminalLocked(const Task& task) noexcept {
    return task.Status == ETaskStatus::COMPLETED || task.Status == ETaskStatus::FAILED ||
           task.Status == ETaskStatus::CANCELLED;
}

bool TaskSystem::TaskLifetime::IsSuccessfulLocked(const Task& task) noexcept {
    return task.Status == ETaskStatus::COMPLETED;
}

void TaskSystem::TaskLifetime::RetireTask(TaskHandle::State& state) noexcept {
    bool expected = false;

    if (!state.RetirementQueued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    TaskHandle::State* head = RetiredTasks.load(std::memory_order_acquire);

    do {
        state.RetiredNext.store(head, std::memory_order_relaxed);
    } while (!RetiredTasks.compare_exchange_weak(head, &state, std::memory_order_release, std::memory_order_acquire));
}

void TaskSystem::TaskLifetime::DrainRetiredTasksLocked(TaskRegistry& registry) {
    TaskHandle::State* retiredState = RetiredTasks.exchange(nullptr, std::memory_order_acquire);

    while (retiredState != nullptr) {
        TaskHandle::State* next = retiredState->RetiredNext.load(std::memory_order_relaxed);
        retiredState->RetiredNext.store(nullptr, std::memory_order_relaxed);

        const auto taskIt = registry.ActiveTasks.find(retiredState->Id);

        if (taskIt != registry.ActiveTasks.end() && taskIt->second.get() == retiredState) {
            auto state = taskIt->second;
            std::lock_guard taskLock{state->Mutex};

            if (IsTerminalLocked(state->Task)) {
                registry.ActiveTasks.erase(taskIt);
            } else {
                state->RetirementQueued.store(false, std::memory_order_release);
            }
        }

        retiredState = next;
    }
}

void TaskSystem::TaskLifetime::ReleaseOutstandingTask() noexcept {
    const std::size_t previousOutstanding = OutstandingTasks.fetch_sub(1, std::memory_order_acq_rel);

    if (previousOutstanding == 1) {
        OutstandingTasks.notify_all();
    }
}

void TaskSystem::CompleteState(const std::shared_ptr<TaskHandle::State>& state,
                               ETaskStatus status,
                               std::optional<ErrorInfo> error) {
    if (status == ETaskStatus::COMPLETED) {
        std::lock_guard taskLock{state->Mutex};

        if (state->Task.Dependents.empty()) {
            state->Task.Error = std::move(error);
            state->Task.Status = status;
            TaskLifetime::ReleaseExecutionPayloadLocked(state->Task);
            state->Condition.notify_all();
            m_lifetime.RetireTask(*state);
            m_lifetime.ReleaseOutstandingTask();
            return;
        }
    }

    std::lock_guard lock{m_mutex};
    std::lock_guard taskLock{state->Mutex};

    if (status == ETaskStatus::COMPLETED) {
        ReadyQueue& readyQueue = *m_readyScheduler.WorkerQueues[CurrentWorker.Index.GetValue()];
        std::lock_guard readyQueueLock{readyQueue.Mutex};
        ReadyScheduler::CompactReadyQueueLocked(readyQueue);
        readyQueue.Tasks.reserve(readyQueue.Tasks.size() + state->Task.Dependents.size());

        std::vector<std::shared_ptr<TaskHandle::State>> readyDependents;
        readyDependents.reserve(state->Task.Dependents.size());
        std::vector<std::uint64_t> dependents = std::move(state->Task.Dependents);

        for (const std::uint64_t dependent: dependents) {
            const auto dependentIt = m_registry.ActiveTasks.find(dependent);

            if (dependentIt == m_registry.ActiveTasks.end()) {
                continue;
            }

            auto& dependentState = dependentIt->second;
            {
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

                if (previousRemaining != 1) {
                    continue;
                }
            }

            readyDependents.push_back(dependentState);
        }

        if (!readyDependents.empty()) {
            state->Task.Error = std::move(error);
            state->Task.Status = status;
            TaskLifetime::ReleaseExecutionPayloadLocked(state->Task);

            for (std::shared_ptr<TaskHandle::State>& dependentState: readyDependents) {
                readyQueue.Tasks.push_back(dependentState);

                {
                    std::lock_guard dependentLock{dependentState->Mutex};
                    dependentState->Task.Status = ETaskStatus::READY;
                }
            }

            m_readyScheduler.ReadyTaskCount.fetch_add(readyDependents.size(), std::memory_order_release);
            m_readyScheduler.Wakeups.release(static_cast<std::ptrdiff_t>(readyDependents.size()));
        } else {
            state->Task.Error = std::move(error);
            state->Task.Status = status;
            TaskLifetime::ReleaseExecutionPayloadLocked(state->Task);
        }
    } else {
        state->Task.Error = std::move(error);
        state->Task.Status = status;
        PropagateCancellationLocked(state->Task);
        TaskLifetime::ReleaseExecutionPayloadLocked(state->Task);
    }

    state->Condition.notify_all();
    m_lifetime.RetireTask(*state);
    m_lifetime.ReleaseOutstandingTask();
}

void TaskSystem::PropagateCancellationLocked(Task& task) {
    for (const std::uint64_t dependent: task.Dependents) {
        const auto dependentIt = m_registry.ActiveTasks.find(dependent);

        if (dependentIt == m_registry.ActiveTasks.end()) {
            continue;
        }

        auto dependentState = dependentIt->second;
        std::lock_guard dependentLock{dependentState->Mutex};
        Task& dependentTask = dependentState->Task;

        if (TaskLifetime::IsTerminalLocked(dependentTask) || dependentTask.Status == ETaskStatus::RUNNING) {
            continue;
        }

        dependentTask.Status = ETaskStatus::CANCELLED;
        PropagateCancellationLocked(dependentTask);
        TaskLifetime::ReleaseExecutionPayloadLocked(dependentTask);
        m_registry.ActiveTasks.erase(dependent);
        dependentState->Condition.notify_all();
        m_lifetime.ReleaseOutstandingTask();
    }
}

void TaskSystem::CancelPendingTasksLocked() noexcept {
    for (auto taskIt = m_registry.ActiveTasks.begin(); taskIt != m_registry.ActiveTasks.end();) {
        auto state = taskIt->second;
        std::lock_guard taskLock{state->Mutex};
        Task& task = state->Task;

        if (task.Status != ETaskStatus::WAITING && task.Status != ETaskStatus::READY) {
            ++taskIt;
            continue;
        }

        task.Status = ETaskStatus::CANCELLED;
        TaskLifetime::ReleaseExecutionPayloadLocked(task);
        taskIt = m_registry.ActiveTasks.erase(taskIt);
        state->Condition.notify_all();
        m_lifetime.ReleaseOutstandingTask();
    }
}

void TaskSystem::TaskLifetime::ReleaseExecutionPayloadLocked(Task& task) {
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
    for (const auto& [taskId, state]: m_registry.ActiveTasks) {
        std::lock_guard taskLock{state->Mutex};
        const Task& task = state->Task;
        std::unordered_set<std::uint64_t> uniqueDependencies;
        std::size_t unfinishedDependencies = 0;

        for (const std::uint64_t dependencyId: task.Dependencies) {
            if (!uniqueDependencies.insert(dependencyId).second) {
                FailDagInvariantLocked("Task has duplicate dependency");
            }

            const auto dependencyIt = m_registry.ActiveTasks.find(dependencyId);

            if (dependencyIt == m_registry.ActiveTasks.end()) {
                continue;
            }

            std::lock_guard dependencyLock{dependencyIt->second->Mutex};

            const std::vector<std::uint64_t>& dependents = dependencyIt->second->Task.Dependents;
            const bool hasBacklink = std::ranges::find(dependents, taskId) != dependents.end();

            if (!hasBacklink) {
                if (dependencyIt->second->Task.Status == ETaskStatus::RUNNING) {
                    if (task.Status == ETaskStatus::WAITING) {
                        ++unfinishedDependencies;
                    }
                    continue;
                }

                if (TaskLifetime::IsSuccessfulLocked(dependencyIt->second->Task)) {
                    continue;
                }
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

            const auto dependentIt = m_registry.ActiveTasks.find(dependentId);

            if (dependentIt == m_registry.ActiveTasks.end()) {
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

    m_readyScheduler.WakeAll(m_workerCount);
    m_lifetime.OutstandingTasks.notify_all();

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
                if (m_stopping.load(std::memory_order_acquire) && !m_readyScheduler.HasReadyTasks()) {
                    return;
                }

                m_readyScheduler.Wakeups.acquire();

                state = m_readyScheduler.TryPopReadyTask(workerIndex);

                if (state && TaskLifetime::TryClaimReadyTask(state, task, function)) {
                    break;
                }

                if (m_stopping.load(std::memory_order_acquire) && !m_readyScheduler.HasReadyTasks()) {
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
                m_lifetime.OutstandingTasks.notify_all();
            }
        }
    }
}

} // namespace NCommon
