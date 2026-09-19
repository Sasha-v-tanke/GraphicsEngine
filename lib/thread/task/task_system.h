#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

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

    [[nodiscard]] bool IsValid() const noexcept {
        return m_id != INVALID_ID;
    }

    [[nodiscard]] std::uint64_t GetId() const noexcept {
        return m_id;
    }

    [[nodiscard]] friend bool operator==(TaskHandle lhs, TaskHandle rhs) noexcept = default;

private:
    static constexpr std::uint64_t INVALID_ID = 0;

    explicit TaskHandle(std::uint64_t id) noexcept
        : m_id(id) {
    }

    std::uint64_t m_id = INVALID_ID;

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
        , m_task(task)
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
    [[nodiscard]] std::exception_ptr GetError(TaskHandle task) const;

    void Cancel(TaskHandle task);
    void Wait(TaskHandle task);
    void WaitIdle();

private:
    struct Task {
        TaskHandle Handle;
        TaskFunction Function;
        ETaskStatus Status = ETaskStatus::CREATED;
        std::vector<TaskHandle> Dependencies;
        std::vector<TaskHandle> Dependents;
        std::size_t PendingDependencies = 0;
        std::exception_ptr Error;
    };

    [[nodiscard]] Task& GetTaskLocked(TaskHandle task);
    [[nodiscard]] const Task& GetTaskLocked(TaskHandle task) const;
    [[nodiscard]] bool IsTerminalLocked(TaskHandle task) const;
    [[nodiscard]] bool IsSuccessfulLocked(TaskHandle task) const;

    void MakeReadyLocked(Task& task);
    void CompleteLocked(TaskHandle task, ETaskStatus status, std::exception_ptr error = nullptr);
    void PropagateCancellationLocked(TaskHandle task);
    void WorkerLoop(WorkerIndex workerIndex) noexcept;

    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::condition_variable m_idleCondition;
    std::vector<Task> m_tasks;
    std::deque<TaskHandle> m_readyTasks;
    std::vector<std::thread> m_workers;
    std::uint64_t m_nextTaskId = 1;
    std::size_t m_runningTasks = 0;
    bool m_stopping = false;
};

} // namespace NCommon
