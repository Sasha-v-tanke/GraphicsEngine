#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/wrapper/non_copyable.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/lib/thread/task/frame_origin.h>

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
    friend class TaskContext;
    friend struct TaskSystemTestAccess;
};

enum class ETaskGroupStatus {
    OPEN,
    SEALED,
    COMPLETED,
    FAILED,
    CANCELLED,
};

class TaskGroupHandle final {
public:
    TaskGroupHandle() = default;
    [[nodiscard]] bool IsValid() const noexcept {
        return m_id != 0 && m_state != nullptr;
    }
    [[nodiscard]] std::uint64_t GetId() const noexcept {
        return m_id;
    }
    [[nodiscard]] friend bool operator==(const TaskGroupHandle&, const TaskGroupHandle&) noexcept = default;

private:
    struct State;
    TaskGroupHandle(std::uint64_t id, std::shared_ptr<State> state) noexcept
        : m_id(id)
        , m_state(std::move(state)) {
    }
    std::uint64_t m_id = 0;
    std::shared_ptr<State> m_state;
    friend class TaskSystem;
    friend class TaskContext;
};

class TaskContext final: public NonTransferable {
public:
    using TaskFunction = std::function<void(TaskContext&)>;

    [[nodiscard]] static std::optional<FrameOrigin> GetFrameOrigin() noexcept {
        return FrameOriginScope::Current();
    }

    [[nodiscard]] WorkerIndex GetWorkerIndex() const noexcept {
        return m_workerIndex;
    }

    // True only for this context on its invoking worker, before the callable returns.
    [[nodiscard]] bool IsCurrentInvocation(const TaskSystem& system) const noexcept;

    [[nodiscard]] TaskHandle GetTask() const noexcept {
        return m_task;
    }

    // Inherits this invocation's group; descendants may spawn after SealGroup.
    TaskHandle SpawnInGroup(TaskFunction function,
                            std::span<const TaskHandle> dependencies = std::span<const TaskHandle>());
    [[nodiscard]] bool IsGroupCancellationRequested() const;

    TaskHandle Spawn(TaskFunction function);
    TaskHandle Spawn(TaskFunction function, std::span<const TaskHandle> dependencies);

private:
    TaskContext(TaskSystem& taskSystem, TaskHandle task, WorkerIndex workerIndex) noexcept
        : m_taskSystem(&taskSystem)
        , m_task(std::move(task))
        , m_workerIndex(workerIndex) {
    }

    // Invocation identity cannot be reassigned or moved out of a running callback.
    TaskSystem* const m_taskSystem = nullptr;
    const TaskHandle m_task;
    const WorkerIndex m_workerIndex{0};

    friend class TaskSystem;
    friend class TaskContext;
};

class TaskSystem final: private NonCopyable {
public:
    using TaskFunction = TaskContext::TaskFunction;

    explicit TaskSystem(std::size_t workerCount = std::thread::hardware_concurrency());
    ~TaskSystem();

    TaskSystem(TaskSystem&&) noexcept = delete;
    TaskSystem& operator=(TaskSystem&&) noexcept = delete;

    TaskHandle Submit(TaskFunction function, std::span<const TaskHandle> dependencies = {}) {
        function = BindFrameOrigin(std::move(function), FrameOriginScope::Current());
        return SubmitImpl(function, dependencies);
    }

    // Open groups count as outstanding work. Seal or cancel every created group before WaitIdle.
    TaskGroupHandle CreateTaskGroup();
    TaskHandle SubmitToGroup(const TaskGroupHandle& group,
                             TaskFunction function,
                             std::span<const TaskHandle> dependencies = std::span<const TaskHandle>());
    TaskHandle SubmitAfterGroup(const TaskGroupHandle& group, TaskFunction function);
    void SealGroup(const TaskGroupHandle& group);
    void CancelGroup(const TaskGroupHandle& group);
    // Host-only; terminal observation includes retirement of every member's execution captures.
    void WaitGroup(const TaskGroupHandle& group);
    [[nodiscard]] ETaskGroupStatus GetGroupStatus(const TaskGroupHandle& group) const;
    [[nodiscard]] std::optional<ErrorInfo> GetGroupError(const TaskGroupHandle& group) const;

    [[nodiscard]] ETaskStatus GetStatus(const TaskHandle& task) const;
    [[nodiscard]] std::optional<ErrorInfo> GetError(const TaskHandle& task) const;
    [[nodiscard]] std::size_t GetWorkerCount() const noexcept {
        return m_workerCount;
    }

    void Cancel(const TaskHandle& task);
    // Terminal observation includes this callable's destruction, but does not join other invocations,
    // including prerequisites of a cancelled task and dynamically spawned children.
    void Wait(const TaskHandle& task);
    void WaitIdle();
    [[nodiscard]] static bool IsCurrentThreadWorker() noexcept;

private:
    struct OwnerToken;
    friend struct TaskHandle::State;
    friend class TaskContext;
    friend struct TaskSystemTestAccess;

    // Storage belongs to the dependent State and is never resized after linking.
    // Both endpoint lists and every hook mutation are protected by m_mutex.
    struct DependencyEdge {
        TaskHandle::State* Prerequisite = nullptr;
        TaskHandle::State* Dependent = nullptr;
        DependencyEdge* PreviousIncoming = nullptr;
        DependencyEdge* NextIncoming = nullptr;
        DependencyEdge* PreviousOutgoing = nullptr;
        DependencyEdge* NextOutgoing = nullptr;
    };

    struct Task {
        Task() = default;
        Task(Task&& other) noexcept
            : Function()
            , Status(other.Status)
            , Dependencies(std::move(other.Dependencies))
            , Incoming(std::exchange(other.Incoming, nullptr))
            , Outgoing(std::exchange(other.Outgoing, nullptr))
            , RemainingDependencies(other.RemainingDependencies.load(std::memory_order_relaxed))
            , Finalizing(other.Finalizing)
            , PendingStatus(other.PendingStatus)
            , Error(std::move(other.Error))
            , Group(std::move(other.Group))
            , GroupGateId(other.GroupGateId) {
#ifndef NDEBUG
            DebugIncomingCount = std::exchange(other.DebugIncomingCount, 0);
            DebugOutgoingCount = std::exchange(other.DebugOutgoingCount, 0);
#endif
            Function.swap(other.Function);
        }
        Task& operator=(Task&& other) noexcept {
            if (this == &other) {
                return *this;
            }

            Function.swap(other.Function);
            Status = other.Status;
            Dependencies = std::move(other.Dependencies);
            Incoming = std::exchange(other.Incoming, nullptr);
            Outgoing = std::exchange(other.Outgoing, nullptr);
            RemainingDependencies.store(other.RemainingDependencies.load(std::memory_order_relaxed),
                                        std::memory_order_relaxed);
            Finalizing = other.Finalizing;
            PendingStatus = other.PendingStatus;
            Error = std::move(other.Error);
            Group = std::move(other.Group);
            GroupGateId = other.GroupGateId;
#ifndef NDEBUG
            DebugIncomingCount = std::exchange(other.DebugIncomingCount, 0);
            DebugOutgoingCount = std::exchange(other.DebugOutgoingCount, 0);
#endif
            return *this;
        }

        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;

        TaskFunction Function;
        ETaskStatus Status = ETaskStatus::CREATED;
        std::vector<DependencyEdge> Dependencies;
        DependencyEdge* Incoming = nullptr;
        DependencyEdge* Outgoing = nullptr;
        std::atomic_size_t RemainingDependencies = 0;
        bool Finalizing = false;
        ETaskStatus PendingStatus = ETaskStatus::CREATED;
        std::optional<ErrorInfo> Error;
        std::shared_ptr<TaskGroupHandle::State> Group;
        std::uint64_t GroupGateId = 0;
#ifndef NDEBUG
        std::size_t DebugIncomingCount = 0;
        std::size_t DebugOutgoingCount = 0;
#endif
    };

    struct ReadyQueue {
        std::mutex Mutex;
        TaskHandle::State* First = nullptr;
        TaskHandle::State* Last = nullptr;
    };

    struct SubmittedTask {
        std::uint64_t Id = 0;
        std::shared_ptr<TaskHandle::State> State;
        Task Task;
    };

    struct TaskRegistry {
        std::shared_ptr<OwnerToken> Owner;
        std::unordered_map<std::uint64_t, std::shared_ptr<TaskHandle::State>> ActiveTasks;
        std::uint64_t NextTaskId = 1;

        TaskRegistry();

        void ValidateHandleOwner(const TaskHandle& task) const;
        [[nodiscard]] std::shared_ptr<TaskHandle::State> AllocateStateLocked();
        void ValidateDependenciesLocked(std::span<const TaskHandle> dependencies) const;
        void ValidateDependenciesMeasuredLocked(std::span<const TaskHandle> dependencies,
                                                std::size_t& comparisons) const;
        template<bool Measure>
        void ValidateDependenciesImplLocked(std::span<const TaskHandle> dependencies, std::size_t* comparisons) const;
        [[nodiscard]] static bool LinkDependencyEdgesLocked(TaskHandle::State& state,
                                                            std::span<const TaskHandle> dependencies) noexcept;
        [[nodiscard]] static std::shared_ptr<TaskHandle::State> PinStateLocked(TaskHandle::State& state) noexcept;
        static void UnlinkEdgeLocked(DependencyEdge& edge) noexcept;
        static void UnlinkDependencyEdgesLocked(Task& task) noexcept;
        void CommitActiveTaskLocked(std::uint64_t taskId, std::shared_ptr<TaskHandle::State> state);
        void RollbackAllocationLocked(std::uint64_t taskId);
    };

    struct ReadyScheduler {
        std::vector<std::unique_ptr<ReadyQueue>> WorkerQueues;
        ReadyQueue InjectedQueue;
        std::mutex WakeMutex;
        std::condition_variable WakeCondition;
        std::atomic_size_t ReadyTaskCount = 0;

        void Initialize(std::size_t workerCount);
        void PublishReadyTask(TaskSystem& owner, const std::shared_ptr<TaskHandle::State>& state) noexcept;
        void RemoveReadyTask(TaskHandle::State& state) noexcept;
        void WaitForWork(const std::atomic_bool& stopping) noexcept;
        [[nodiscard]] std::shared_ptr<TaskHandle::State> TryPopReadyTask(WorkerIndex workerIndex);
        [[nodiscard]] bool HasReadyTasks() const noexcept;
        void WakeAll() noexcept;
        void UnlinkReadyTaskLocked(ReadyQueue& queue, TaskHandle::State& state) noexcept;
        [[nodiscard]] std::shared_ptr<TaskHandle::State> PopReadyTask(ReadyQueue& queue, bool fromBack);

    private:
        [[nodiscard]] std::shared_ptr<TaskHandle::State> TryPopLocalReadyTask(WorkerIndex workerIndex);
        [[nodiscard]] std::shared_ptr<TaskHandle::State> TryPopInjectedReadyTask();
        [[nodiscard]] std::shared_ptr<TaskHandle::State> TryStealReadyTask(WorkerIndex workerIndex);
    };

    struct RetiredBatch {
        std::shared_ptr<TaskHandle::State> First;
        ~RetiredBatch();
    };

    struct TaskLifetime {
        std::atomic<TaskHandle::State*> RetiredTasks = nullptr;
        std::atomic_size_t OutstandingTasks = 0;

        [[nodiscard]] static bool IsTerminalLocked(const Task& task) noexcept;
        [[nodiscard]] static bool IsSuccessfulLocked(const Task& task) noexcept;
        [[nodiscard]] static bool
        TryClaimReadyTask(const std::shared_ptr<TaskHandle::State>& state, TaskHandle& task, TaskFunction& function);
        static void ReleaseDependencyPayloadLocked(Task& task) noexcept;
        void RetireTask(TaskHandle::State& state) noexcept;
        void DrainRetiredTasksLocked(TaskRegistry& registry, RetiredBatch& retired);
        void ReleaseOutstandingTask() noexcept;
    };

    // The batch owns an acyclic State chain and drains it outside all executor locks.
    class FinalizationBatch final {
    public:
        explicit FinalizationBatch(TaskSystem& owner) noexcept;
        ~FinalizationBatch();
        FinalizationBatch(const FinalizationBatch&) = delete;
        FinalizationBatch& operator=(const FinalizationBatch&) = delete;
        void Add(const std::shared_ptr<TaskHandle::State>& state) noexcept;

        std::shared_ptr<TaskHandle::State> First;
        TaskHandle::State* Last = nullptr;
        TaskHandle::State* FirstUnprepared = nullptr;
        TaskHandle::State* LastUnprepared = nullptr;

    private:
        TaskSystem& m_owner;
    };

    void ValidateTaskHandleOwner(const TaskHandle& task) const;

    void CompleteState(const std::shared_ptr<TaskHandle::State>& state,
                       ETaskStatus status,
                       std::optional<ErrorInfo> error,
                       TaskFunction& function) noexcept;
    void PublishSuccessLocked(TaskHandle::State& state) noexcept;
    static void CancelTaskLocked(const std::shared_ptr<TaskHandle::State>& state, FinalizationBatch& batch) noexcept;
    static void PropagateCancellationLocked(Task& task, FinalizationBatch& batch) noexcept;
    void PrepareCancellationsLocked(FinalizationBatch& batch) noexcept;
    void CancelPendingTasksLocked(FinalizationBatch& batch) noexcept;
    void DrainFinalizations(FinalizationBatch& batch) noexcept;
    [[noreturn]] static void FailDagInvariantLocked(const char* message) noexcept;
    static void ValidateMutationLocked(const TaskHandle::State& state) noexcept;
    static void ValidateEdgeLocked(const DependencyEdge& edge) noexcept;
#ifndef NDEBUG
    struct DagValidationStats {
        std::size_t MutationStates = 0;
        std::size_t MutationEdges = 0;
        std::size_t FullStates = 0;
        std::size_t FullEdges = 0;
    };
    [[nodiscard]] static const char* FindMutationViolationLocked(const TaskHandle::State& state) noexcept;
    [[nodiscard]] const char* FindDagViolationFullLocked() const;
    void ValidateDagFullLocked() const;
    [[nodiscard]] DagValidationStats GetDagValidationStatsLocked() const noexcept;
#endif
    void StopWorkers() noexcept;
    void WorkerLoop(WorkerIndex workerIndex) noexcept;

    TaskHandle SubmitImpl(TaskFunction& function,
                          std::span<const TaskHandle> dependencies,
                          const std::shared_ptr<TaskGroupHandle::State>& group = nullptr,
                          bool descendant = false);
    void ValidateGroup(const TaskGroupHandle& group) const;
    static void QueueGroupCompletionLocked(TaskGroupHandle::State& group, FinalizationBatch& batch) noexcept;
    void RetireGroupMemberLocked(const std::shared_ptr<TaskHandle::State>& state, FinalizationBatch& batch) noexcept;
    void CloseGroupLocked(TaskGroupHandle::State& group, FinalizationBatch& batch) noexcept;
    void PublishGroupGateLocked(TaskHandle::State& state, FinalizationBatch& batch) noexcept;

private:
    mutable std::mutex m_mutex;
    const std::size_t m_workerCount;
    TaskRegistry m_registry;
    std::unordered_map<std::uint64_t, std::shared_ptr<TaskGroupHandle::State>> m_groups;
    ReadyScheduler m_readyScheduler;
    TaskLifetime m_lifetime;
    std::vector<std::thread> m_workers;
    std::atomic_bool m_stopping = false;
};

} // namespace NCommon
