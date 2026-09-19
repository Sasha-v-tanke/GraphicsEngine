#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include <lib/common/error/error.h>
#include <lib/common/wrapper/non_copyable.h>

namespace NCommon {

enum class ETaskStatus {
    CREATED,
    WAITING,
    READY,
    RUNNING,
    COMPLETED,
    FAILED,
    CANCELLED,
};

class TaskSystem;

class WorkerIndex final {
public:
    explicit constexpr WorkerIndex(std::size_t value) noexcept
        : m_value(value) {
    }

    [[nodiscard]] constexpr std::size_t GetValue() const noexcept {
        return m_value;
    }

    [[nodiscard]] constexpr explicit operator std::size_t() const noexcept {
        return m_value;
    }

    [[nodiscard]] friend constexpr bool operator==(WorkerIndex lhs, WorkerIndex rhs) noexcept = default;

private:
    std::size_t m_value = 0;
};

class TaskHandle final {
public:
    TaskHandle() = default;

    [[nodiscard]] bool IsValid() const noexcept;

    [[nodiscard]] std::uint64_t GetId() const noexcept {
        return m_id;
    }

    [[nodiscard]] friend bool operator==(TaskHandle lhs, TaskHandle rhs) noexcept = default;

private:
    struct State;

    explicit TaskHandle(std::uint64_t id, std::shared_ptr<State> state) noexcept
        : m_id(id)
        , m_state(std::move(state)) {
    }

    std::uint64_t m_id = 0;
    std::shared_ptr<State> m_state;

    friend class TaskSystem;
};

class TaskContext final {
public:
    using TaskFunction = std::function<void(TaskContext&)>;

    [[nodiscard]] WorkerIndex GetWorkerIndex() const noexcept {
        return m_workerIndex;
    }

    [[nodiscard]] TaskHandle GetTask() const noexcept {
        return m_task;
    }

    TaskHandle Spawn(TaskFunction function);

private:
    TaskContext(TaskSystem& taskSystem, TaskHandle task, WorkerIndex workerIndex) noexcept
        : m_taskSystem(&taskSystem)
        , m_task(std::move(task))
        , m_workerIndex(workerIndex) {
    }

    TaskSystem* m_taskSystem = nullptr;
    TaskHandle m_task;
    WorkerIndex m_workerIndex{0};

    friend class TaskSystem;
};

class TaskSystem final: private NonCopyable {
public:
    using TaskFunction = TaskContext::TaskFunction;

    explicit TaskSystem(std::size_t workerCount = std::thread::hardware_concurrency());
    ~TaskSystem();

    TaskSystem(TaskSystem&&) noexcept = delete;
    TaskSystem& operator=(TaskSystem&&) noexcept = delete;

    TaskHandle Submit(TaskFunction function, std::span<const TaskHandle> dependencies = {});

    [[nodiscard]] ETaskStatus GetStatus(TaskHandle task) const;
    [[nodiscard]] std::optional<ErrorInfo> GetError(TaskHandle task) const;

    void Cancel(TaskHandle task);
    void Wait(TaskHandle task);
    void WaitIdle();

private:
    friend struct TaskHandle::State;

    struct Task {
        TaskHandle Handle;
        TaskFunction Function;
        ETaskStatus Status = ETaskStatus::CREATED;
        std::vector<TaskHandle> Dependencies;
        std::vector<TaskHandle> Dependents;
        std::size_t PendingDependencies = 0;
        std::optional<ErrorInfo> Error;
    };

    [[nodiscard]] Task& GetTaskLocked(TaskHandle task);
    [[nodiscard]] const Task& GetTaskLocked(TaskHandle task) const;
    [[nodiscard]] bool IsTerminalLocked(const Task& task) const noexcept;
    [[nodiscard]] bool IsSuccessfulLocked(const Task& task) const noexcept;

    void MakeReadyLocked(Task& task);
    void CompleteLocked(TaskHandle task, ETaskStatus status, std::optional<ErrorInfo> error = std::nullopt);
    void PropagateCancellationLocked(Task& task);
    void ReleaseExecutionPayloadLocked(Task& task);
    void StopWorkers() noexcept;
    void WorkerLoop(WorkerIndex workerIndex) noexcept;

    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::condition_variable m_idleCondition;
    std::unordered_map<std::uint64_t, std::weak_ptr<TaskHandle::State>> m_activeTasks;
    std::deque<TaskHandle> m_readyTasks;
    std::vector<std::thread> m_workers;
    std::uint64_t m_nextTaskId = 1;
    std::size_t m_runningTasks = 0;
    bool m_stopping = false;
};

} // namespace NCommon
