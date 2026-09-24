#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
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

    [[nodiscard]] friend bool operator==(const TaskHandle& lhs, const TaskHandle& rhs) noexcept = default;

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
    TaskHandle Spawn(TaskFunction function, std::span<const TaskHandle> dependencies);

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

    TaskHandle Submit(TaskFunction function, std::span<const TaskHandle> dependencies = {}) {
        return SubmitImpl(std::move(function), dependencies);
    }

    [[nodiscard]] ETaskStatus GetStatus(const TaskHandle& task) const;
    [[nodiscard]] std::optional<ErrorInfo> GetError(const TaskHandle& task) const;
    [[nodiscard]] std::size_t GetWorkerCount() const noexcept {
        return m_workerCount;
    }

    void Cancel(const TaskHandle& task);
    void Wait(const TaskHandle& task);
    void WaitIdle();

private:
    struct OwnerToken;
    friend struct TaskHandle::State;

    struct Task {
        Task() = default;
        Task(Task&& other) noexcept
            : Function(std::move(other.Function))
            , Status(other.Status)
            , Dependencies(std::move(other.Dependencies))
            , Dependents(std::move(other.Dependents))
            , RemainingDependencies(other.RemainingDependencies.load(std::memory_order_relaxed))
            , Error(std::move(other.Error)) {
        }
        Task& operator=(Task&& other) noexcept {
            if (this == &other) {
                return *this;
            }

            Function = std::move(other.Function);
            Status = other.Status;
            Dependencies = std::move(other.Dependencies);
            Dependents = std::move(other.Dependents);
            RemainingDependencies.store(other.RemainingDependencies.load(std::memory_order_relaxed),
                                        std::memory_order_relaxed);
            Error = std::move(other.Error);
            return *this;
        }

        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;

        TaskFunction Function;
        ETaskStatus Status = ETaskStatus::CREATED;
        std::vector<std::uint64_t> Dependencies;
        std::vector<std::uint64_t> Dependents;
        std::atomic_size_t RemainingDependencies = 0;
        std::optional<ErrorInfo> Error;
    };

    struct ReadyQueue {
        std::mutex Mutex;
        std::vector<std::shared_ptr<TaskHandle::State>> Tasks;
        std::size_t Head = 0;
    };

    struct SubmittedTask {
        std::uint64_t Id = 0;
        std::shared_ptr<TaskHandle::State> State;
        Task Task;
        std::vector<std::uint64_t> LinkedDependencies;
        bool OutstandingCommitted = false;
    };

    struct LinkedDependencies {
        bool Cancelled = false;
        std::vector<std::uint64_t> LinkedIds;
    };

    struct TaskRegistry {
        std::shared_ptr<OwnerToken> Owner;
        std::unordered_map<std::uint64_t, std::shared_ptr<TaskHandle::State>> ActiveTasks;
        std::uint64_t NextTaskId = 1;

        TaskRegistry();

        void ValidateHandleOwner(const TaskHandle& task) const;
        [[nodiscard]] std::shared_ptr<TaskHandle::State> AllocateStateLocked();
        void ValidateDependenciesLocked(std::span<const TaskHandle> dependencies) const;
        [[nodiscard]] LinkedDependencies
        LinkDependencyEdgesLocked(std::uint64_t taskId, Task& task, std::span<const TaskHandle> dependencies);
        void RollbackDependencyEdgesLocked(std::uint64_t taskId, std::span<const std::uint64_t> dependencyIds);
        void CommitActiveTaskLocked(std::uint64_t taskId, std::shared_ptr<TaskHandle::State> state);
        void RollbackAllocationLocked(std::uint64_t taskId);
    };

    struct ReadyScheduler {
        std::vector<std::unique_ptr<ReadyQueue>> WorkerQueues;
        ReadyQueue InjectedQueue;
        std::counting_semaphore<> Wakeups{0};
        std::atomic_size_t ReadyTaskCount = 0;

        void Initialize(std::size_t workerCount);
        void PublishReadyTask(TaskSystem& owner, std::shared_ptr<TaskHandle::State> state);
        [[nodiscard]] std::shared_ptr<TaskHandle::State> TryPopReadyTask(WorkerIndex workerIndex);
        [[nodiscard]] bool HasReadyTasks() const noexcept;
        void WakeAll(std::size_t workerCount);
        static void CompactReadyQueueLocked(ReadyQueue& queue);

    private:
        [[nodiscard]] std::shared_ptr<TaskHandle::State> TryPopLocalReadyTask(WorkerIndex workerIndex);
        [[nodiscard]] std::shared_ptr<TaskHandle::State> TryPopInjectedReadyTask();
        [[nodiscard]] std::shared_ptr<TaskHandle::State> TryStealReadyTask(WorkerIndex workerIndex);
    };

    struct TaskLifetime {
        std::atomic<TaskHandle::State*> RetiredTasks = nullptr;
        std::atomic_size_t OutstandingTasks = 0;

        [[nodiscard]] static bool IsTerminalLocked(const Task& task) noexcept;
        [[nodiscard]] static bool IsSuccessfulLocked(const Task& task) noexcept;
        [[nodiscard]] static bool
        TryClaimReadyTask(const std::shared_ptr<TaskHandle::State>& state, TaskHandle& task, TaskFunction& function);
        static void ReleaseExecutionPayloadLocked(Task& task);
        void RetireTask(TaskHandle::State& state) noexcept;
        void DrainRetiredTasksLocked(TaskRegistry& registry);
        void ReleaseOutstandingTask() noexcept;
    };

    void ValidateTaskHandleOwner(const TaskHandle& task) const;

    void CompleteState(const std::shared_ptr<TaskHandle::State>& state,
                       ETaskStatus status,
                       std::optional<ErrorInfo> error = std::nullopt);
    void PropagateCancellationLocked(Task& task);
    void CancelPendingTasksLocked() noexcept;
    [[noreturn]] static void FailDagInvariantLocked(const char* message) noexcept;
#ifndef NDEBUG
    void ValidateDagLocked() const;
#endif
    void StopWorkers() noexcept;
    void WorkerLoop(WorkerIndex workerIndex) noexcept;

    TaskHandle SubmitImpl(TaskFunction function, std::span<const TaskHandle> dependencies);

private:
    mutable std::mutex m_mutex;
    const std::size_t m_workerCount;
    TaskRegistry m_registry;
    ReadyScheduler m_readyScheduler;
    TaskLifetime m_lifetime;
    std::vector<std::thread> m_workers;
    std::atomic_bool m_stopping = false;
};

} // namespace NCommon
