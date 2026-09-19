#include "task_system.h"

#include <algorithm>
#include <utility>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NCommon {

namespace {

struct WorkerThreadContext {
    TaskSystem* System = nullptr;
    WorkerIndex Index{0};
};

thread_local WorkerThreadContext CurrentWorker;

} // namespace

TaskHandle TaskContext::Spawn(TaskFunction function) {
    return m_taskSystem->Submit(std::move(function));
}

TaskSystem::TaskSystem(std::size_t workerCount) {
    if (workerCount == 0) {
        workerCount = 1;
    }

    m_workers.reserve(workerCount);

    for (std::size_t index = 0; index < workerCount; ++index) {
        m_workers.emplace_back([this, workerIndex = WorkerIndex{index}] { WorkerLoop(workerIndex); });
    }
}

TaskSystem::~TaskSystem() {
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

    // Validate the complete dependency set before modifying the graph.
    for (std::size_t index = 0; index < task.Dependencies.size(); ++index) {
        const TaskHandle dependency = task.Dependencies[index];

        static_cast<void>(GetTaskLocked(dependency));

        if (std::ranges::find(
                    task.Dependencies.begin(),
                    task.Dependencies.begin() + static_cast<std::ptrdiff_t>(index),
                    dependency) !=
            task.Dependencies.begin() + static_cast<std::ptrdiff_t>(index)) {
            GRAPHICS_ENGINE_THROW(
                    EError::INVALID_ARGUMENT,
                    "Task {} is specified more than once as a dependency",
                    dependency.GetId());
        }
    }

    // The task becomes publishable only after the complete dependency set
    // has been validated.
    task.Handle = TaskHandle{m_nextTaskId++};

    for (TaskHandle dependency: task.Dependencies) {
        Task& dependencyTask = GetTaskLocked(dependency);

        if (!IsTerminalLocked(dependency)) {
            ++task.PendingDependencies;
            dependencyTask.Dependents.push_back(task.Handle);
        } else if (!IsSuccessfulLocked(dependency)) {
            task.Status = ETaskStatus::CANCELLED;
        }
    }

    if (task.Status != ETaskStatus::CANCELLED) {
        task.Status = task.PendingDependencies == 0 ? ETaskStatus::READY : ETaskStatus::WAITING;
    }

    const TaskHandle handle = task.Handle;

    m_tasks.push_back(std::move(task));

    Task& storedTask = m_tasks.back();

    if (storedTask.Status == ETaskStatus::READY) {
        MakeReadyLocked(storedTask);
    } else if (storedTask.Status == ETaskStatus::CANCELLED) {
        m_idleCondition.notify_all();
    }

    return handle;
}

ETaskStatus TaskSystem::GetStatus(TaskHandle task) const {
    std::lock_guard lock{m_mutex};

    return GetTaskLocked(task).Status;
}

std::exception_ptr TaskSystem::GetError(TaskHandle task) const {
    std::lock_guard lock{m_mutex};

    return GetTaskLocked(task).Error;
}

void TaskSystem::Cancel(TaskHandle task) {
    std::lock_guard lock{m_mutex};

    Task& storedTask = GetTaskLocked(task);

    if (IsTerminalLocked(task)) {
        return;
    }

    if (storedTask.Status == ETaskStatus::RUNNING) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Running task cannot be cancelled");
    }

    CompleteLocked(task, ETaskStatus::CANCELLED);
}

void TaskSystem::Wait(TaskHandle task) {
    if (CurrentWorker.System == this) {
        GRAPHICS_ENGINE_THROW(
                EError::INVALID_STATE,
                "TaskSystem::Wait cannot be called from a worker thread of the same TaskSystem");
    }

    std::unique_lock lock{m_mutex};

    static_cast<void>(GetTaskLocked(task));

    m_idleCondition.wait(lock, [this, task] { return IsTerminalLocked(task); });
}

void TaskSystem::WaitIdle() {
    if (CurrentWorker.System == this) {
        GRAPHICS_ENGINE_THROW(
                EError::INVALID_STATE,
                "TaskSystem::WaitIdle cannot be called from a worker thread of the same TaskSystem");
    }

    std::unique_lock lock{m_mutex};

    m_idleCondition.wait(lock, [this] {
        return m_readyTasks.empty() && m_runningTasks == 0 &&
               std::ranges::all_of(m_tasks, [this](const Task& task) { return IsTerminalLocked(task.Handle); });
    });
}

TaskSystem::Task& TaskSystem::GetTaskLocked(TaskHandle task) {
    if (!task.IsValid()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Invalid task handle");
    }

    const auto it = std::ranges::find_if(m_tasks, [task](const Task& storedTask) { return storedTask.Handle == task; });

    if (it == m_tasks.end()) {
        GRAPHICS_ENGINE_THROW(EError::NOT_FOUND, "Task {} was not found", task.GetId());
    }

    return *it;
}

const TaskSystem::Task& TaskSystem::GetTaskLocked(TaskHandle task) const {
    if (!task.IsValid()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Invalid task handle");
    }

    const auto it = std::ranges::find_if(m_tasks, [task](const Task& storedTask) { return storedTask.Handle == task; });

    if (it == m_tasks.end()) {
        GRAPHICS_ENGINE_THROW(EError::NOT_FOUND, "Task {} was not found", task.GetId());
    }

    return *it;
}

bool TaskSystem::IsTerminalLocked(TaskHandle task) const {
    const ETaskStatus status = GetTaskLocked(task).Status;

    return status == ETaskStatus::COMPLETED || status == ETaskStatus::FAILED || status == ETaskStatus::CANCELLED;
}

bool TaskSystem::IsSuccessfulLocked(TaskHandle task) const {
    return GetTaskLocked(task).Status == ETaskStatus::COMPLETED;
}

void TaskSystem::MakeReadyLocked(Task& task) {
    task.Status = ETaskStatus::READY;
    m_readyTasks.push_back(task.Handle);
    m_condition.notify_one();
}

void TaskSystem::CompleteLocked(TaskHandle task, ETaskStatus status, std::exception_ptr error) {
    Task& storedTask = GetTaskLocked(task);
    storedTask.Status = status;
    storedTask.Error = std::move(error);

    if (status != ETaskStatus::COMPLETED) {
        PropagateCancellationLocked(task);
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

    m_idleCondition.notify_all();
}

void TaskSystem::PropagateCancellationLocked(TaskHandle task) {
    for (TaskHandle dependent: GetTaskLocked(task).Dependents) {
        Task& dependentTask = GetTaskLocked(dependent);

        if (IsTerminalLocked(dependent) || dependentTask.Status == ETaskStatus::RUNNING) {
            continue;
        }

        dependentTask.Status = ETaskStatus::CANCELLED;
        PropagateCancellationLocked(dependent);
    }
}

void TaskSystem::WorkerLoop(WorkerIndex workerIndex) noexcept {
    CurrentWorker.System = this;
    CurrentWorker.Index = workerIndex;

    while (true) {
        TaskHandle task;
        TaskFunction function;

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
            function = storedTask.Function;
            ++m_runningTasks;
        }

        std::exception_ptr error;

        try {
            TaskContext context{*this, task, workerIndex};
            function(context);
        } catch (...) {
            error = std::current_exception();
        }

        {
            std::lock_guard lock{m_mutex};

            --m_runningTasks;
            CompleteLocked(
                task,
                error == nullptr ? ETaskStatus::COMPLETED : ETaskStatus::FAILED,
                std::move(error));
        }
    }
}

} // namespace NCommon
