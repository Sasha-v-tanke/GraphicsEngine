#include "task_system.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <lib/common/error/exception.h>

namespace NCommon {

namespace {

struct WorkerThreadContext {
    TaskSystem* System = nullptr;
    WorkerIndex Index{0};
};

thread_local WorkerThreadContext CurrentWorker;

[[nodiscard]] ErrorInfo MakeFailureError(std::exception_ptr error) {
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
}

} // namespace

struct TaskHandle::State {
    TaskSystem* Owner = nullptr;
    std::uint64_t Id = 0;
    TaskSystem::Task Task;
};

bool TaskHandle::IsValid() const noexcept {
    return m_id != 0 && m_state != nullptr;
}

TaskHandle TaskContext::Spawn(TaskFunction function) {
    return m_taskSystem->Submit(std::move(function));
}

TaskSystem::TaskSystem(std::size_t workerCount) {
    if (workerCount == 0) {
        workerCount = 1;
    }

    m_workers.reserve(workerCount);

    try {
        for (std::size_t index = 0; index < workerCount; ++index) {
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

TaskHandle TaskSystem::Submit(TaskFunction function, std::span<const TaskHandle> dependencies) {
    if (!function) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Task function is empty");
    }

    std::lock_guard lock{m_mutex};

    if (m_stopping) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "TaskSystem is stopping");
    }

    Task task;
    task.Function = std::move(function);
    task.Dependencies.assign(dependencies.begin(), dependencies.end());

    for (std::size_t index = 0; index < task.Dependencies.size(); ++index) {
        const TaskHandle dependency = task.Dependencies[index];

        static_cast<void>(GetTaskLocked(dependency));

        if (std::ranges::find(task.Dependencies.begin(),
                              task.Dependencies.begin() + static_cast<std::ptrdiff_t>(index),
                              dependency) != task.Dependencies.begin() + static_cast<std::ptrdiff_t>(index)) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT,
                                  "Task {} is specified more than once as a dependency",
                                  dependency.GetId());
        }
    }

    const std::uint64_t id = m_nextTaskId++;
    auto state = std::make_shared<TaskHandle::State>();
    state->Owner = this;
    state->Id = id;
    task.Handle = TaskHandle{id, state};

    for (TaskHandle dependency: task.Dependencies) {
        Task& dependencyTask = GetTaskLocked(dependency);

        if (!IsTerminalLocked(dependencyTask)) {
            ++task.PendingDependencies;
            dependencyTask.Dependents.push_back(task.Handle);
        } else if (!IsSuccessfulLocked(dependencyTask)) {
            task.Status = ETaskStatus::CANCELLED;
        }
    }

    if (task.Status != ETaskStatus::CANCELLED) {
        task.Status = task.PendingDependencies == 0 ? ETaskStatus::READY : ETaskStatus::WAITING;
    }

    const TaskHandle handle = task.Handle;
    state->Task = std::move(task);
    m_activeTasks.emplace(id, state);

    Task& storedTask = state->Task;

    if (storedTask.Status == ETaskStatus::READY) {
        MakeReadyLocked(storedTask);
    } else if (storedTask.Status == ETaskStatus::CANCELLED) {
        ReleaseExecutionPayloadLocked(storedTask);
        m_activeTasks.erase(storedTask.Handle.GetId());
        m_idleCondition.notify_all();
    }

    return handle;
}

ETaskStatus TaskSystem::GetStatus(TaskHandle task) const {
    std::lock_guard lock{m_mutex};

    return GetTaskLocked(task).Status;
}

std::optional<ErrorInfo> TaskSystem::GetError(TaskHandle task) const {
    std::lock_guard lock{m_mutex};

    return GetTaskLocked(task).Error;
}

void TaskSystem::Cancel(TaskHandle task) {
    std::lock_guard lock{m_mutex};

    Task& storedTask = GetTaskLocked(task);

    if (IsTerminalLocked(storedTask)) {
        return;
    }

    if (storedTask.Status == ETaskStatus::RUNNING) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Running task cannot be cancelled");
    }

    CompleteLocked(task, ETaskStatus::CANCELLED);
}

void TaskSystem::Wait(TaskHandle task) {
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

    m_idleCondition.wait(lock, [this] { return m_readyTasks.empty() && m_runningTasks == 0 && m_activeTasks.empty(); });
}

TaskSystem::Task& TaskSystem::GetTaskLocked(TaskHandle task) {
    if (!task.IsValid()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Invalid task handle");
    }

    const auto state = task.m_state;

    if (state->Owner != this || state->Id != task.GetId()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Task {} belongs to another TaskSystem", task.GetId());
    }

    return state->Task;
}

const TaskSystem::Task& TaskSystem::GetTaskLocked(TaskHandle task) const {
    if (!task.IsValid()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Invalid task handle");
    }

    const auto state = task.m_state;

    if (state->Owner != this || state->Id != task.GetId()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Task {} belongs to another TaskSystem", task.GetId());
    }

    return state->Task;
}

bool TaskSystem::IsTerminalLocked(const Task& task) const noexcept {
    return task.Status == ETaskStatus::COMPLETED || task.Status == ETaskStatus::FAILED ||
           task.Status == ETaskStatus::CANCELLED;
}

bool TaskSystem::IsSuccessfulLocked(const Task& task) const noexcept {
    return task.Status == ETaskStatus::COMPLETED;
}

void TaskSystem::MakeReadyLocked(Task& task) {
    task.Status = ETaskStatus::READY;
    m_readyTasks.push_back(task.Handle);
    m_condition.notify_one();
}

void TaskSystem::CompleteLocked(TaskHandle task, ETaskStatus status, std::optional<ErrorInfo> error) {
    Task& storedTask = GetTaskLocked(task);
    storedTask.Status = status;
    storedTask.Error = std::move(error);

    if (status != ETaskStatus::COMPLETED) {
        PropagateCancellationLocked(storedTask);
        ReleaseExecutionPayloadLocked(storedTask);
        m_activeTasks.erase(storedTask.Handle.GetId());
        m_idleCondition.notify_all();
        return;
    }

    for (TaskHandle dependent: storedTask.Dependents) {
        Task& dependentTask = GetTaskLocked(dependent);

        if (dependentTask.Status != ETaskStatus::WAITING) {
            continue;
        }

        --dependentTask.PendingDependencies;

        if (dependentTask.PendingDependencies == 0) {
            MakeReadyLocked(dependentTask);
        }
    }

    ReleaseExecutionPayloadLocked(storedTask);
    m_activeTasks.erase(storedTask.Handle.GetId());
    m_idleCondition.notify_all();
}

void TaskSystem::PropagateCancellationLocked(Task& task) {
    for (TaskHandle dependent: task.Dependents) {
        Task& dependentTask = GetTaskLocked(dependent);

        if (IsTerminalLocked(dependentTask) || dependentTask.Status == ETaskStatus::RUNNING) {
            continue;
        }

        dependentTask.Status = ETaskStatus::CANCELLED;
        PropagateCancellationLocked(dependentTask);
        ReleaseExecutionPayloadLocked(dependentTask);
        m_activeTasks.erase(dependentTask.Handle.GetId());
    }
}

void TaskSystem::ReleaseExecutionPayloadLocked(Task& task) {
    task.Function = {};
    task.Dependencies.clear();
    task.Dependents.clear();
    task.PendingDependencies = 0;
}

void TaskSystem::StopWorkers() noexcept {
    {
        std::lock_guard lock{m_mutex};
        m_stopping = true;
    }

    m_condition.notify_all();

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
        TaskHandle task;
        TaskFunction function;

        try {
            {
                std::unique_lock lock{m_mutex};

                m_condition.wait(lock, [this] { return m_stopping || !m_readyTasks.empty(); });

                if (m_stopping && m_readyTasks.empty()) {
                    return;
                }

                task = m_readyTasks.front();
                m_readyTasks.pop_front();

                Task& storedTask = GetTaskLocked(task);

                if (storedTask.Status != ETaskStatus::READY) {
                    continue;
                }

                storedTask.Status = ETaskStatus::RUNNING;
                function = std::move(storedTask.Function);
                ++m_runningTasks;
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
                CompleteLocked(task,
                               error.has_value() ? ETaskStatus::FAILED : ETaskStatus::COMPLETED,
                               std::move(error));
            }
        } catch (...) {
            if (task.IsValid()) {
                std::lock_guard lock{m_mutex};

                CompleteLocked(task, ETaskStatus::FAILED, MakeFailureError(std::current_exception()));
            }
        }
    }
}

} // namespace NCommon
