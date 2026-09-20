#include "task_system.h"

#include <algorithm>
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
    m_workers.reserve(m_workerCount);

    try {
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

    if (m_stopping) {
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

    try {
        linkedDependencies.reserve(dependencies.size());

        for (const TaskHandle& dependency: dependencies) {
            Task& dependencyTask = GetTaskLocked(dependency);

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
            MakeReadyLocked(id, storedTask);
        } else if (storedTask.Status == ETaskStatus::CANCELLED) {
            ReleaseExecutionPayloadLocked(storedTask);
            m_activeTasks.erase(id);
            m_idleCondition.notify_all();
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

        if (m_nextTaskId == id + 1) {
            --m_nextTaskId;
        }

        throw;
    }
}

ETaskStatus TaskSystem::GetStatus(const TaskHandle& task) const {
    std::lock_guard lock{m_mutex};

    return GetTaskLocked(task).Status;
}

std::optional<ErrorInfo> TaskSystem::GetError(const TaskHandle& task) const {
    std::lock_guard lock{m_mutex};

    return GetTaskLocked(task).Error;
}

void TaskSystem::Cancel(const TaskHandle& task) {
    std::lock_guard lock{m_mutex};

    Task& storedTask = GetTaskLocked(task);

    if (IsTerminalLocked(storedTask)) {
        return;
    }

    if (storedTask.Status == ETaskStatus::RUNNING) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Running task cannot be cancelled");
    }

    CompleteLocked(task.GetId(), ETaskStatus::CANCELLED);
}

void TaskSystem::Wait(const TaskHandle& task) {
    if (CurrentWorker.System == this) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE,
                              "TaskSystem::Wait cannot be called from a worker thread of the same TaskSystem");
    }

    std::unique_lock lock{m_mutex};

    static_cast<void>(GetTaskLocked(task));

    m_idleCondition.wait(lock, [this, task] { return IsTerminalLocked(GetTaskLocked(task)); });
}

void TaskSystem::WaitIdle() {
    if (CurrentWorker.System == this) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE,
                              "TaskSystem::WaitIdle cannot be called from a worker thread of the same TaskSystem");
    }

    std::unique_lock lock{m_mutex};

    m_idleCondition.wait(lock, [this] { return m_runningTasks == 0 && m_activeTasks.empty(); });
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

TaskSystem::Task& TaskSystem::GetTaskLocked(std::uint64_t taskId) {
    const auto it = m_activeTasks.find(taskId);

    if (it == m_activeTasks.end()) {
        GRAPHICS_ENGINE_THROW(EError::NOT_FOUND, "Task {} was not found", taskId);
    }

    return it->second->Task;
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
    m_readyTasks.push_back(taskId);
    m_condition.notify_one();
}

void TaskSystem::CompleteLocked(std::uint64_t taskId, ETaskStatus status, std::optional<ErrorInfo> error) {
    Task& storedTask = GetTaskLocked(taskId);
    storedTask.Status = status;
    storedTask.Error = std::move(error);

    if (status != ETaskStatus::COMPLETED) {
        PropagateCancellationLocked(storedTask);
        ReleaseExecutionPayloadLocked(storedTask);
        m_activeTasks.erase(taskId);
#ifndef NDEBUG
        ValidateDagLocked();
#endif
        m_idleCondition.notify_all();
        return;
    }

    for (const std::uint64_t dependent: storedTask.Dependents) {
        const auto dependentIt = m_activeTasks.find(dependent);

        if (dependentIt == m_activeTasks.end()) {
            continue;
        }

        Task& dependentTask = dependentIt->second->Task;

        if (dependentTask.Status != ETaskStatus::WAITING) {
            continue;
        }

        const std::size_t previousRemaining =
                dependentTask.RemainingDependencies.fetch_sub(1, std::memory_order_acq_rel);

        if (previousRemaining == 0) {
            FailDagInvariantLocked("Task dependency counter underflow while completing prerequisite");
        }

        if (previousRemaining == 1) {
            MakeReadyLocked(dependent, dependentTask);
        }
    }

    ReleaseExecutionPayloadLocked(storedTask);
    m_activeTasks.erase(taskId);
#ifndef NDEBUG
    ValidateDagLocked();
#endif
    m_idleCondition.notify_all();
}

void TaskSystem::PropagateCancellationLocked(Task& task) {
    for (const std::uint64_t dependent: task.Dependents) {
        const auto dependentIt = m_activeTasks.find(dependent);

        if (dependentIt == m_activeTasks.end()) {
            continue;
        }

        Task& dependentTask = dependentIt->second->Task;

        if (IsTerminalLocked(dependentTask) || dependentTask.Status == ETaskStatus::RUNNING) {
            continue;
        }

        dependentTask.Status = ETaskStatus::CANCELLED;
        PropagateCancellationLocked(dependentTask);
        ReleaseExecutionPayloadLocked(dependentTask);
        m_activeTasks.erase(dependent);
    }
}

void TaskSystem::CancelPendingTasksLocked() noexcept {
    for (auto taskIt = m_activeTasks.begin(); taskIt != m_activeTasks.end();) {
        Task& task = taskIt->second->Task;

        if (task.Status != ETaskStatus::WAITING && task.Status != ETaskStatus::READY) {
            ++taskIt;
            continue;
        }

        task.Status = ETaskStatus::CANCELLED;
        ReleaseExecutionPayloadLocked(task);
        taskIt = m_activeTasks.erase(taskIt);
    }
}

void TaskSystem::ReleaseExecutionPayloadLocked(Task& task) {
    task.Function = {};
    task.Dependencies.clear();
    task.Dependents.clear();
    task.RemainingDependencies.store(0, std::memory_order_relaxed);
}

[[noreturn]] void TaskSystem::FailDagInvariantLocked([[maybe_unused]] const char* message) noexcept {
    std::terminate();
}

#ifndef NDEBUG
void TaskSystem::ValidateDagLocked() const {
    for (const auto& [taskId, state]: m_activeTasks) {
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

            const std::vector<std::uint64_t>& dependents = dependencyIt->second->Task.Dependents;

            if (std::ranges::find(dependents, taskId) == dependents.end()) {
                FailDagInvariantLocked("Task dependency does not link back to dependent");
            }

            if (!IsSuccessfulLocked(dependencyIt->second->Task)) {
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
        m_stopping = true;
        CancelPendingTasksLocked();
    }

    m_condition.notify_all();
    m_idleCondition.notify_all();

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
        std::uint64_t taskId = 0;
        bool taskIsRunning = false;
        TaskHandle task;
        TaskFunction function;

        try {
            {
                std::unique_lock lock{m_mutex};

                m_condition.wait(lock, [this] { return m_stopping || !m_readyTasks.empty(); });

                if (m_stopping && m_readyTasks.empty()) {
                    return;
                }

                taskId = m_readyTasks.front();
                m_readyTasks.pop_front();

                const auto stateIt = m_activeTasks.find(taskId);

                if (stateIt == m_activeTasks.end()) {
                    m_idleCondition.notify_all();
                    continue;
                }

                Task& storedTask = stateIt->second->Task;

                if (storedTask.Status != ETaskStatus::READY) {
                    m_idleCondition.notify_all();
                    continue;
                }

                storedTask.Status = ETaskStatus::RUNNING;
                task = TaskHandle{taskId, stateIt->second};
                ++m_runningTasks;
                taskIsRunning = true;
                function = std::move(storedTask.Function);
            }

            std::optional<ErrorInfo> error;

            try {
                TaskContext context{*this, task, workerIndex};
                function(context);
            } catch (...) {
                error = MakeFailureError(std::current_exception());
            }

            {
                std::lock_guard lock{m_mutex};

                --m_runningTasks;
                taskIsRunning = false;
                CompleteLocked(taskId,
                               error.has_value() ? ETaskStatus::FAILED : ETaskStatus::COMPLETED,
                               std::move(error));
            }
        } catch (...) {
            std::lock_guard lock{m_mutex};

            if (taskIsRunning) {
                --m_runningTasks;
                taskIsRunning = false;
            }

            if (taskId != 0 && m_activeTasks.contains(taskId)) {
                CompleteLocked(taskId, ETaskStatus::FAILED, MakeFailureError(std::current_exception()));
            } else {
                m_idleCondition.notify_all();
            }
        }
    }
}

} // namespace NCommon
