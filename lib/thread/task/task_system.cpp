#include "task_system.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
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

struct TaskHandle::State: std::enable_shared_from_this<TaskHandle::State> {
    mutable std::mutex Mutex;
    std::condition_variable Condition;
    std::shared_ptr<const TaskSystem::OwnerToken> Owner;
    std::atomic<TaskHandle::State*> RetiredNext = nullptr;
    std::atomic_bool RetirementQueued = false;
    std::uint64_t Id = 0;
    std::shared_ptr<State> FinalizationNext;
    State* CancellationNext = nullptr;
    std::atomic<TaskSystem::ReadyQueue*> Queue = nullptr;
    State* ReadyPrevious = nullptr;
    State* ReadyNext = nullptr;
    TaskSystem::Task Task;
};

struct TaskGroupHandle::State {
    std::shared_ptr<const void> Owner;
    TaskHandle Completion;
    std::size_t Outstanding = 0;
    bool Sealed = false;
    bool CompletionQueued = false;
    ETaskStatus Outcome = ETaskStatus::COMPLETED;
    TaskHandle FirstFailure;
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
    if (NextTaskId == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Task ID exhausted");
    }
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

bool TaskSystem::TaskRegistry::LinkDependencyEdgesLocked(TaskHandle::State& state,
                                                         std::span<const TaskHandle> dependencies) noexcept {
    Task& task = state.Task;
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        TaskHandle::State& prerequisite = *dependencies[index].m_state;
        bool isTerminal = false;
        bool isSuccessful = false;
        {
            std::lock_guard dependencyLock{prerequisite.Mutex};
            const Task& dependencyTask = prerequisite.Task;
            isTerminal = TaskLifetime::IsTerminalLocked(dependencyTask);
            isSuccessful = isTerminal
                                 ? TaskLifetime::IsSuccessfulLocked(dependencyTask)
                                 : !dependencyTask.Finalizing || dependencyTask.PendingStatus == ETaskStatus::COMPLETED;
        }
        if (!isSuccessful) {
            UnlinkDependencyEdgesLocked(task);
            task.RemainingDependencies.store(0, std::memory_order_relaxed);
            return true;
        }
        if (isTerminal) {
            continue;
        }
        DependencyEdge& edge = task.Dependencies[index];
        edge.Prerequisite = &prerequisite;
        edge.Dependent = &state;
        edge.NextIncoming = task.Incoming;
        if (task.Incoming != nullptr) {
            task.Incoming->PreviousIncoming = &edge;
        }
        task.Incoming = &edge;
        edge.NextOutgoing = prerequisite.Task.Outgoing;
        if (prerequisite.Task.Outgoing != nullptr) {
            prerequisite.Task.Outgoing->PreviousOutgoing = &edge;
        }
        prerequisite.Task.Outgoing = &edge;
        task.RemainingDependencies.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

std::shared_ptr<TaskHandle::State> TaskSystem::TaskRegistry::PinStateLocked(TaskHandle::State& state) noexcept {
    std::shared_ptr<TaskHandle::State> pin = state.weak_from_this().lock();
    if (pin == nullptr) {
        FailDagInvariantLocked("Linked dependency endpoint has no strong owner");
    }
    return pin;
}

void TaskSystem::TaskRegistry::UnlinkEdgeLocked(DependencyEdge& edge) noexcept {
    if (edge.Prerequisite == nullptr) {
        return;
    }
    if (edge.PreviousIncoming != nullptr) {
        edge.PreviousIncoming->NextIncoming = edge.NextIncoming;
    } else {
        edge.Dependent->Task.Incoming = edge.NextIncoming;
    }
    if (edge.NextIncoming != nullptr) {
        edge.NextIncoming->PreviousIncoming = edge.PreviousIncoming;
    }
    if (edge.PreviousOutgoing != nullptr) {
        edge.PreviousOutgoing->NextOutgoing = edge.NextOutgoing;
    } else {
        edge.Prerequisite->Task.Outgoing = edge.NextOutgoing;
    }
    if (edge.NextOutgoing != nullptr) {
        edge.NextOutgoing->PreviousOutgoing = edge.PreviousOutgoing;
    }
    edge = {};
}

void TaskSystem::TaskRegistry::UnlinkDependencyEdgesLocked(Task& task) noexcept {
    while (task.Incoming != nullptr) {
        UnlinkEdgeLocked(*task.Incoming);
    }
}

void TaskSystem::TaskRegistry::CommitActiveTaskLocked(std::uint64_t taskId, std::shared_ptr<TaskHandle::State> state) {
    ActiveTasks.emplace(taskId, std::move(state));
}

void TaskSystem::TaskRegistry::RollbackAllocationLocked(std::uint64_t taskId) {
    ActiveTasks.erase(taskId);
}

bool TaskHandle::IsValid() const noexcept {
    return m_id != 0 && m_state != nullptr;
}

TaskHandle TaskContext::Spawn(TaskFunction function) {
    function = BindFrameOrigin(std::move(function), GetFrameOrigin());
    return m_taskSystem->SubmitImpl(function, {});
}

TaskHandle TaskContext::Spawn(TaskFunction function, std::span<const TaskHandle> dependencies) {
    function = BindFrameOrigin(std::move(function), GetFrameOrigin());
    return m_taskSystem->SubmitImpl(function, dependencies);
}

TaskHandle TaskContext::SpawnInGroup(TaskFunction function, std::span<const TaskHandle> dependencies) {
    const std::shared_ptr<TaskGroupHandle::State>& group = m_task.m_state->Task.Group;
    if (!group) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Invocation does not belong to a task group");
    }
    function = BindFrameOrigin(std::move(function), GetFrameOrigin());
    return m_taskSystem->SubmitImpl(function, dependencies, group, true);
}

bool TaskContext::IsGroupCancellationRequested() const {
    std::lock_guard lock{m_taskSystem->m_mutex};
    const std::shared_ptr<TaskGroupHandle::State>& group = m_task.m_state->Task.Group;
    return group && (group->Outcome != ETaskStatus::COMPLETED || m_taskSystem->m_stopping.load());
}

void TaskSystem::ValidateGroup(const TaskGroupHandle& group) const {
    if (!group.IsValid() || group.m_state->Owner != m_registry.Owner ||
        group.GetId() != group.m_state->Completion.GetId()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Invalid or foreign task group");
    }
}

TaskGroupHandle TaskSystem::CreateTaskGroup() {
    auto group = std::make_shared<TaskGroupHandle::State>();
    group->Owner = m_registry.Owner;
    std::lock_guard lock{m_mutex};
    if (m_stopping.load(std::memory_order_acquire)) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "TaskSystem is stopping");
    }
    auto state = m_registry.AllocateStateLocked();
    state->Task.Status = ETaskStatus::WAITING;
    state->Task.GroupGateId = state->Id;
    group->Completion = TaskHandle{state->Id, state};
    try {
        m_registry.CommitActiveTaskLocked(state->Id, state);
        m_groups.emplace(state->Id, group);
    } catch (...) {
        m_registry.RollbackAllocationLocked(state->Id);
        throw;
    }
    m_lifetime.OutstandingTasks.fetch_add(1, std::memory_order_release);
    return TaskGroupHandle{state->Id, std::move(group)};
}

TaskHandle TaskSystem::SubmitToGroup(const TaskGroupHandle& group,
                                     TaskFunction function,
                                     std::span<const TaskHandle> dependencies) {
    ValidateGroup(group);
    function = BindFrameOrigin(std::move(function), FrameOriginScope::Current());
    return SubmitImpl(function, dependencies, group.m_state);
}

TaskHandle TaskSystem::SubmitAfterGroup(const TaskGroupHandle& group, TaskFunction function) {
    ValidateGroup(group);
    function = BindFrameOrigin(std::move(function), FrameOriginScope::Current());
    return SubmitImpl(function, std::span{&group.m_state->Completion, 1});
}

void TaskSystem::QueueGroupCompletionLocked(TaskGroupHandle::State& group, FinalizationBatch& batch) noexcept {
    if (!group.Sealed || group.Outstanding != 0 || group.CompletionQueued) {
        return;
    }
    group.CompletionQueued = true;
    const std::shared_ptr<TaskHandle::State>& gate = group.Completion.m_state;
    {
        std::lock_guard taskLock{gate->Mutex};
        gate->Task.Finalizing = true;
        gate->Task.PendingStatus = group.Outcome;
    }
    batch.Add(gate);
}

void TaskSystem::CloseGroupLocked(TaskGroupHandle::State& group, FinalizationBatch& batch) noexcept {
    group.Sealed = true;
    for (const auto& [id, member]: m_registry.ActiveTasks) {
        static_cast<void>(id);
        if (member->Task.Group.get() == &group) {
            CancelTaskLocked(member, batch);
        }
    }
    PrepareCancellationsLocked(batch);
    QueueGroupCompletionLocked(group, batch);
}

void TaskSystem::SealGroup(const TaskGroupHandle& group) {
    ValidateGroup(group);
    FinalizationBatch finalizations{*this};
    std::lock_guard lock{m_mutex};
    group.m_state->Sealed = true;
    QueueGroupCompletionLocked(*group.m_state, finalizations);
}

void TaskSystem::CancelGroup(const TaskGroupHandle& group) {
    ValidateGroup(group);
    FinalizationBatch finalizations{*this};
    std::lock_guard lock{m_mutex};
    if (group.m_state->CompletionQueued) {
        return;
    }
    if (group.m_state->Outcome != ETaskStatus::FAILED) {
        group.m_state->Outcome = ETaskStatus::CANCELLED;
    }
    CloseGroupLocked(*group.m_state, finalizations);
}

void TaskSystem::WaitGroup(const TaskGroupHandle& group) {
    if (IsCurrentThreadWorker()) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "WaitGroup is host-only");
    }
    ValidateGroup(group);
    Wait(group.m_state->Completion);
}

ETaskGroupStatus TaskSystem::GetGroupStatus(const TaskGroupHandle& group) const {
    ValidateGroup(group);
    std::lock_guard lock{m_mutex};
    switch (GetStatus(group.m_state->Completion)) {
    case ETaskStatus::COMPLETED:
        return ETaskGroupStatus::COMPLETED;
    case ETaskStatus::FAILED:
        return ETaskGroupStatus::FAILED;
    case ETaskStatus::CANCELLED:
        return ETaskGroupStatus::CANCELLED;
    default:
        return group.m_state->Sealed ? ETaskGroupStatus::SEALED : ETaskGroupStatus::OPEN;
    }
}

std::optional<ErrorInfo> TaskSystem::GetGroupError(const TaskGroupHandle& group) const {
    ValidateGroup(group);
    std::lock_guard lock{m_mutex};
    if (!TaskLifetime::IsTerminalLocked(group.m_state->Completion.m_state->Task) ||
        !group.m_state->FirstFailure.IsValid()) {
        return std::nullopt;
    }
    return GetError(group.m_state->FirstFailure);
}

void TaskSystem::RetireGroupMemberLocked(const std::shared_ptr<TaskHandle::State>& state,
                                         FinalizationBatch& batch) noexcept {
    std::shared_ptr<TaskGroupHandle::State> group = std::move(state->Task.Group);
    if (!group) {
        return;
    }
    if (state->Task.Status == ETaskStatus::FAILED && !group->FirstFailure.IsValid()) {
        group->FirstFailure = TaskHandle{state->Id, state};
        group->Outcome = ETaskStatus::FAILED;
        CloseGroupLocked(*group, batch);
    } else if (state->Task.Status == ETaskStatus::CANCELLED && group->Outcome == ETaskStatus::COMPLETED) {
        group->Outcome = ETaskStatus::CANCELLED;
        CloseGroupLocked(*group, batch);
    }
    --group->Outstanding;
    QueueGroupCompletionLocked(*group, batch);
}

void TaskSystem::PublishGroupGateLocked(TaskHandle::State& state, FinalizationBatch& batch) noexcept {
    // The gate is preallocated at group creation. Fan-in and ready publication cannot allocate.
    if (state.Task.PendingStatus == ETaskStatus::COMPLETED) {
        PublishSuccessLocked(state);
    } else {
        PropagateCancellationLocked(state.Task, batch);
        PrepareCancellationsLocked(batch);
    }
    m_groups.erase(state.Task.GroupGateId);
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

TaskHandle TaskSystem::SubmitImpl(TaskFunction& function,
                                  std::span<const TaskHandle> dependencies,
                                  const std::shared_ptr<TaskGroupHandle::State>& group,
                                  bool descendant) {
    if (!function) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_ARGUMENT, "Task function is empty");
    }

    // Keep every possible last callable owner outside the registry lock's lifetime, including rollback.
    FinalizationBatch finalizations{*this};
    RetiredBatch retired;
    SubmittedTask submitted;
    Task task;
    task.Function.swap(function);
    task.Dependencies.resize(dependencies.size());

    std::unique_lock lock{m_mutex};
    m_lifetime.DrainRetiredTasksLocked(m_registry, retired);
    if (m_stopping.load(std::memory_order_acquire)) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "TaskSystem is stopping");
    }
    if (group &&
        (group->CompletionQueued || group->Outcome != ETaskStatus::COMPLETED || (group->Sealed && !descendant))) {
        GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Task group admission is closed");
    }
    m_registry.ValidateDependenciesLocked(dependencies);
    submitted.Task = std::move(task);
    submitted.State = m_registry.AllocateStateLocked();
    submitted.Id = submitted.State->Id;

    // The registry insertion is the last throwing preparation. Nothing can observe it while G
    // is held; state/callable owners declared before the lock survive any insertion failure.
#ifndef NDEBUG
    ValidateDagLocked();
#endif
    submitted.State->Task = std::move(submitted.Task);
    m_registry.CommitActiveTaskLocked(submitted.Id, submitted.State);

    // Commit: graph links, accounting and ready publication below cannot allocate.
    const bool cancelled = TaskRegistry::LinkDependencyEdgesLocked(*submitted.State, dependencies);
    submitted.State->Task.Status = submitted.State->Task.RemainingDependencies.load(std::memory_order_relaxed) == 0
                                         ? ETaskStatus::READY
                                         : ETaskStatus::WAITING;
    m_lifetime.OutstandingTasks.fetch_add(1, std::memory_order_release);
    if (group) {
        ++group->Outstanding;
        submitted.State->Task.Group = group;
    }
    if (cancelled) {
        CancelTaskLocked(submitted.State, finalizations);
        PrepareCancellationsLocked(finalizations);
    } else if (submitted.State->Task.Status == ETaskStatus::READY) {
        m_readyScheduler.PublishReadyTask(*this, submitted.State);
    }
    lock.unlock();
    m_readyScheduler.WakeAll();
    return TaskHandle{submitted.Id, submitted.State};
}

ETaskStatus TaskSystem::GetStatus(const TaskHandle& task) const {
    ValidateTaskHandleOwner(task);

    std::lock_guard taskLock{task.m_state->Mutex};
    return task.m_state->Task.Status;
}

std::optional<ErrorInfo> TaskSystem::GetError(const TaskHandle& task) const {
    ValidateTaskHandleOwner(task);

    std::lock_guard taskLock{task.m_state->Mutex};
    return TaskLifetime::IsTerminalLocked(task.m_state->Task) ? task.m_state->Task.Error : std::nullopt;
}

void TaskSystem::Cancel(const TaskHandle& task) {
    FinalizationBatch finalizations{*this};
    std::lock_guard lock{m_mutex};
    ValidateTaskHandleOwner(task);
    {
        std::lock_guard taskLock{task.m_state->Mutex};
        const Task& storedTask = task.m_state->Task;
        if (TaskLifetime::IsTerminalLocked(storedTask) || storedTask.Finalizing) {
            return;
        }
        if (storedTask.Status == ETaskStatus::RUNNING) {
            GRAPHICS_ENGINE_THROW(EError::INVALID_STATE, "Running task cannot be cancelled");
        }
    }
    CancelTaskLocked(task.m_state, finalizations);
    PrepareCancellationsLocked(finalizations);
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

bool TaskSystem::IsCurrentThreadWorker() noexcept {
    return CurrentWorker.System != nullptr;
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

    RetiredBatch retired;
    std::lock_guard lock{m_mutex};
    m_lifetime.DrainRetiredTasksLocked(m_registry, retired);
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

void TaskSystem::ReadyScheduler::PublishReadyTask(TaskSystem& owner,
                                                  const std::shared_ptr<TaskHandle::State>& state) noexcept {
    ReadyQueue& queue = CurrentWorker.System == &owner ? *WorkerQueues[CurrentWorker.Index.GetValue()] : InjectedQueue;
    std::lock_guard queueLock{queue.Mutex};
    if (state->Queue.load(std::memory_order_relaxed) != nullptr) {
        FailDagInvariantLocked("Duplicate READY membership");
    }
    state->ReadyPrevious = queue.Last;
    if (queue.Last != nullptr) {
        queue.Last->ReadyNext = state.get();
    } else {
        queue.First = state.get();
    }
    queue.Last = state.get();
    state->Queue.store(&queue, std::memory_order_release);
    ReadyTaskCount.fetch_add(1, std::memory_order_release);
}

void TaskSystem::ReadyScheduler::UnlinkReadyTaskLocked(ReadyQueue& queue, TaskHandle::State& state) noexcept {
    if (state.ReadyPrevious != nullptr) {
        state.ReadyPrevious->ReadyNext = state.ReadyNext;
    } else {
        queue.First = state.ReadyNext;
    }
    if (state.ReadyNext != nullptr) {
        state.ReadyNext->ReadyPrevious = state.ReadyPrevious;
    } else {
        queue.Last = state.ReadyPrevious;
    }
    state.ReadyPrevious = nullptr;
    state.ReadyNext = nullptr;
    state.Queue.store(nullptr, std::memory_order_release);
    if (ReadyTaskCount.fetch_sub(1, std::memory_order_acq_rel) == 0) {
        FailDagInvariantLocked("READY counter underflow");
    }
}

void TaskSystem::ReadyScheduler::RemoveReadyTask(TaskHandle::State& state) noexcept {
    ReadyQueue* queue = state.Queue.load(std::memory_order_acquire);
    if (queue == nullptr) {
        return;
    }
    std::lock_guard queueLock{queue->Mutex};
    if (state.Queue.load(std::memory_order_relaxed) == queue) {
        UnlinkReadyTaskLocked(*queue, state);
    }
}

std::shared_ptr<TaskHandle::State> TaskSystem::ReadyScheduler::PopReadyTask(ReadyQueue& queue, bool fromBack) {
    std::lock_guard queueLock{queue.Mutex};
    TaskHandle::State* raw = fromBack ? queue.Last : queue.First;
    if (raw == nullptr) {
        return {};
    }
    // Registry owns every queued node. Acquire a lease before unlink, including stale pops;
    // cancellation/retirement cannot reclaim it between Q unlock and the separate S claim.
    std::shared_ptr<TaskHandle::State> state = TaskRegistry::PinStateLocked(*raw);
    UnlinkReadyTaskLocked(queue, *state);
    return state;
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

void TaskSystem::ReadyScheduler::WakeAll() noexcept {
    // W never nests with G/S/Q. Pair the predicate change with a W handshake so a worker
    // cannot miss publication between checking the atomic predicate and going to sleep.
    std::lock_guard lock{WakeMutex};
    WakeCondition.notify_all();
}

void TaskSystem::ReadyScheduler::WaitForWork(const std::atomic_bool& stopping) noexcept {
    std::unique_lock lock{WakeMutex};
    WakeCondition.wait(lock, [&] { return HasReadyTasks() || stopping.load(std::memory_order_acquire); });
}

std::shared_ptr<TaskHandle::State> TaskSystem::ReadyScheduler::TryPopLocalReadyTask(WorkerIndex workerIndex) {
    return PopReadyTask(*WorkerQueues[workerIndex.GetValue()], true);
}

std::shared_ptr<TaskHandle::State> TaskSystem::ReadyScheduler::TryPopInjectedReadyTask() {
    return PopReadyTask(InjectedQueue, false);
}

std::shared_ptr<TaskHandle::State> TaskSystem::ReadyScheduler::TryStealReadyTask(WorkerIndex workerIndex) {
    for (std::size_t offset = 1; offset < WorkerQueues.size(); ++offset) {
        const std::size_t victimIndex = (workerIndex.GetValue() + offset) % WorkerQueues.size();
        if (auto state = PopReadyTask(*WorkerQueues[victimIndex], false)) {
            return state;
        }
    }
    return {};
}

bool TaskSystem::TaskLifetime::TryClaimReadyTask(const std::shared_ptr<TaskHandle::State>& state,
                                                 TaskHandle& task,
                                                 TaskFunction& function) {
    std::lock_guard taskLock{state->Mutex};

    if (state->Task.Status != ETaskStatus::READY || state->Task.Finalizing) {
        return false;
    }

    state->Task.Status = ETaskStatus::RUNNING;
    task = TaskHandle{state->Id, state};
    function.swap(state->Task.Function);
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

TaskSystem::RetiredBatch::~RetiredBatch() {
    // Drop registry owners after G, iteratively so large retired graphs do not recurse.
    while (First != nullptr) {
        std::shared_ptr<TaskHandle::State> state = std::move(First);
        First = std::move(state->FinalizationNext);
    }
}

void TaskSystem::TaskLifetime::DrainRetiredTasksLocked(TaskRegistry& registry, RetiredBatch& retired) {
    TaskHandle::State* retiredState = RetiredTasks.exchange(nullptr, std::memory_order_acquire);

    while (retiredState != nullptr) {
        TaskHandle::State* next = retiredState->RetiredNext.load(std::memory_order_relaxed);
        retiredState->RetiredNext.store(nullptr, std::memory_order_relaxed);

        const auto taskIt = registry.ActiveTasks.find(retiredState->Id);

        if (taskIt != registry.ActiveTasks.end() && taskIt->second.get() == retiredState) {
            auto state = taskIt->second;
            std::lock_guard taskLock{state->Mutex};

            if (IsTerminalLocked(state->Task)) {
                state->FinalizationNext = std::move(retired.First);
                retired.First = state;
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

TaskSystem::FinalizationBatch::FinalizationBatch(TaskSystem& owner) noexcept
    : m_owner(owner) {
}

TaskSystem::FinalizationBatch::~FinalizationBatch() {
    m_owner.DrainFinalizations(*this);
}

void TaskSystem::FinalizationBatch::Add(const std::shared_ptr<TaskHandle::State>& state) noexcept {
    if (Last != nullptr) {
        Last->FinalizationNext = state;
    } else {
        First = state;
    }
    Last = state.get();
    if (state->Task.GroupGateId == 0 && state->Task.PendingStatus == ETaskStatus::CANCELLED) {
        if (LastUnprepared != nullptr) {
            LastUnprepared->CancellationNext = state.get();
        } else {
            FirstUnprepared = state.get();
        }
        LastUnprepared = state.get();
    }
}

void TaskSystem::DrainFinalizations(FinalizationBatch& batch) noexcept {
    while (batch.First != nullptr) {
        std::shared_ptr<TaskHandle::State> state = std::move(batch.First);
        batch.First = std::move(state->FinalizationNext);
        if (!batch.First) {
            batch.Last = nullptr;
        }
        TaskFunction function;
        {
            std::lock_guard taskLock{state->Mutex};
            function.swap(state->Task.Function);
        }
        function = {}; // Captures may call back into this pool, including during StopWorkers.
        {
            std::lock_guard lock{m_mutex};
            if (state->Task.GroupGateId != 0) {
                PublishGroupGateLocked(*state, batch);
            }
            {
                std::lock_guard taskLock{state->Mutex};
                TaskLifetime::ReleaseDependencyPayloadLocked(state->Task);
                state->Task.Status = state->Task.PendingStatus;
            }
            RetireGroupMemberLocked(state, batch);
            m_lifetime.RetireTask(*state);
            m_lifetime.ReleaseOutstandingTask();
        }
        state->Condition.notify_all();
        m_readyScheduler.WakeAll();
    }
    batch.Last = nullptr;
}

void TaskSystem::PublishSuccessLocked(TaskHandle::State& state) noexcept {
    while (state.Task.Outgoing != nullptr) {
        DependencyEdge& edge = *state.Task.Outgoing;
        const std::shared_ptr<TaskHandle::State> dependent = TaskRegistry::PinStateLocked(*edge.Dependent);
        TaskRegistry::UnlinkEdgeLocked(edge);
        bool ready = false;
        {
            std::lock_guard taskLock{dependent->Mutex};
            Task& task = dependent->Task;
            if (task.Status != ETaskStatus::WAITING || task.Finalizing) {
                continue;
            }
            const std::size_t remaining = task.RemainingDependencies.fetch_sub(1, std::memory_order_acq_rel);
            if (remaining == 0) {
                FailDagInvariantLocked("Task dependency counter underflow");
            }
            if (remaining == 1) {
                task.Status = ETaskStatus::READY;
                ready = true;
            }
        }
        if (ready) {
            m_readyScheduler.PublishReadyTask(*this, dependent);
        }
    }
}

void TaskSystem::CompleteState(const std::shared_ptr<TaskHandle::State>& state,
                               ETaskStatus status,
                               std::optional<ErrorInfo> error,
                               TaskFunction& function) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<std::optional<ErrorInfo>>);
    FinalizationBatch finalizations{*this};
    {
        std::lock_guard lock{m_mutex};
        std::lock_guard taskLock{state->Mutex};
        if (state->Task.Finalizing || TaskLifetime::IsTerminalLocked(state->Task)) {
            return;
        }
        state->Task.Finalizing = true;
        state->Task.PendingStatus = status;
        state->Task.Error = std::move(error);
    }
    function = {};
    {
        std::lock_guard lock{m_mutex};
        // G closes outgoing admission atomically with the public terminal transition.
        {
            std::lock_guard taskLock{state->Mutex};
            state->Task.Status = status;
        }
        if (status == ETaskStatus::COMPLETED) {
            PublishSuccessLocked(*state);
        } else {
            PropagateCancellationLocked(state->Task, finalizations);
            PrepareCancellationsLocked(finalizations);
        }
        {
            std::lock_guard taskLock{state->Mutex};
            TaskLifetime::ReleaseDependencyPayloadLocked(state->Task);
        }
        RetireGroupMemberLocked(state, finalizations);
        m_lifetime.RetireTask(*state);
        m_lifetime.ReleaseOutstandingTask();
    }
    state->Condition.notify_all();
    m_readyScheduler.WakeAll();
}

void TaskSystem::CancelTaskLocked(const std::shared_ptr<TaskHandle::State>& state, FinalizationBatch& batch) noexcept {
    std::lock_guard taskLock{state->Mutex};
    Task& task = state->Task;
    if (task.Finalizing || TaskLifetime::IsTerminalLocked(task) || task.Status == ETaskStatus::RUNNING) {
        return;
    }
    task.Finalizing = true;
    task.PendingStatus = ETaskStatus::CANCELLED;
    batch.Add(state);
}

void TaskSystem::PropagateCancellationLocked(Task& task, FinalizationBatch& batch) noexcept {
    while (task.Outgoing != nullptr) {
        DependencyEdge& edge = *task.Outgoing;
        const std::shared_ptr<TaskHandle::State> dependent = TaskRegistry::PinStateLocked(*edge.Dependent);
        TaskRegistry::UnlinkEdgeLocked(edge);
        CancelTaskLocked(dependent, batch);
    }
}

void TaskSystem::PrepareCancellationsLocked(FinalizationBatch& batch) noexcept {
    // The pending graph-work queue is separate from capture retirement. Reentrant group cleanup
    // can append work without revisiting the already detached portion of the batch.
    while (batch.FirstUnprepared != nullptr) {
        TaskHandle::State* state = batch.FirstUnprepared;
        batch.FirstUnprepared = std::exchange(state->CancellationNext, nullptr);
        if (batch.FirstUnprepared == nullptr) {
            batch.LastUnprepared = nullptr;
        }
        m_readyScheduler.RemoveReadyTask(*state);
        TaskRegistry::UnlinkDependencyEdgesLocked(state->Task);
        state->Task.RemainingDependencies.store(0, std::memory_order_relaxed);
        PropagateCancellationLocked(state->Task, batch);
    }
}

void TaskSystem::CancelPendingTasksLocked(FinalizationBatch& batch) noexcept {
    for (const auto& [id, state]: m_registry.ActiveTasks) {
        static_cast<void>(id);
        if (state->Task.GroupGateId == 0) {
            CancelTaskLocked(state, batch);
        }
    }
    PrepareCancellationsLocked(batch);
}

void TaskSystem::TaskLifetime::ReleaseDependencyPayloadLocked(Task& task) noexcept {
    if (task.Incoming != nullptr || task.Outgoing != nullptr) {
        FailDagInvariantLocked("Retiring a task with linked dependency edges");
    }
    task.Dependencies.clear();
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
        ETaskStatus status;
        {
            std::lock_guard taskLock{state->Mutex};
            if (state->Task.Finalizing || TaskLifetime::IsTerminalLocked(state->Task)) {
                continue;
            }
            status = state->Task.Status;
        }
        const Task& task = state->Task;
        std::unordered_set<std::uint64_t> uniqueDependencies;
        std::size_t unfinishedDependencies = 0;
        const DependencyEdge* previous = nullptr;
        for (const DependencyEdge* edge = task.Incoming; edge != nullptr; edge = edge->NextIncoming) {
            if (edge->Dependent != state.get() || edge->Prerequisite == nullptr || edge->PreviousIncoming != previous ||
                !uniqueDependencies.insert(edge->Prerequisite->Id).second) {
                FailDagInvariantLocked("Invalid incoming dependency edge");
            }
            if ((edge->PreviousOutgoing == nullptr && edge->Prerequisite->Task.Outgoing != edge) ||
                (edge->PreviousOutgoing != nullptr && edge->PreviousOutgoing->NextOutgoing != edge) ||
                (edge->NextOutgoing != nullptr && edge->NextOutgoing->PreviousOutgoing != edge)) {
                FailDagInvariantLocked("Dependency edge does not link back from prerequisite");
            }
            ++unfinishedDependencies;
            previous = edge;
        }
        if (status == ETaskStatus::WAITING &&
            task.RemainingDependencies.load(std::memory_order_relaxed) != unfinishedDependencies) {
            FailDagInvariantLocked("Task remaining dependency counter is inconsistent");
        }
        std::unordered_set<std::uint64_t> uniqueDependents;
        previous = nullptr;
        for (const DependencyEdge* edge = task.Outgoing; edge != nullptr; edge = edge->NextOutgoing) {
            if (edge->Prerequisite != state.get() || edge->Dependent == nullptr || edge->PreviousOutgoing != previous ||
                !uniqueDependents.insert(edge->Dependent->Id).second) {
                FailDagInvariantLocked("Invalid outgoing dependency edge");
            }
            if ((edge->PreviousIncoming == nullptr && edge->Dependent->Task.Incoming != edge) ||
                (edge->PreviousIncoming != nullptr && edge->PreviousIncoming->NextIncoming != edge) ||
                (edge->NextIncoming != nullptr && edge->NextIncoming->PreviousIncoming != edge)) {
                FailDagInvariantLocked("Dependency edge does not link back from dependent");
            }
            previous = edge;
        }
    }
}
#endif

void TaskSystem::StopWorkers() noexcept {
    {
        FinalizationBatch finalizations{*this};
        std::lock_guard lock{m_mutex};
        m_stopping.store(true, std::memory_order_release);
        for (const auto& [id, group]: m_groups) {
            static_cast<void>(id);
            group->Outcome = group->Outcome == ETaskStatus::FAILED ? ETaskStatus::FAILED : ETaskStatus::CANCELLED;
            group->Sealed = true;
            QueueGroupCompletionLocked(*group, finalizations);
        }
        CancelPendingTasksLocked(finalizations);
    }

    m_readyScheduler.WakeAll();
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
        m_readyScheduler.WaitForWork(m_stopping);
        std::shared_ptr<TaskHandle::State> state;
        do {
            state = m_readyScheduler.TryPopReadyTask(workerIndex);
        } while (state == nullptr && m_readyScheduler.HasReadyTasks());
        if (!state) {
            if (m_stopping.load(std::memory_order_acquire)) {
                return;
            }
            continue;
        }
        TaskHandle task;
        TaskFunction function;
        if (!TaskLifetime::TryClaimReadyTask(state, task, function)) {
            continue;
        }
        std::optional<ErrorInfo> error;
        // Only invocation failures are task failures. The terminal tail cannot allocate and
        // internal primitive/invariant failures are fail-stop, never a second completion.
        try {
            TaskContext context{*this, task, workerIndex};
            function(context);
        } catch (...) {
            error = MakeFailureError(std::current_exception());
        }
        CompleteState(state,
                      error.has_value() ? ETaskStatus::FAILED : ETaskStatus::COMPLETED,
                      std::move(error),
                      function);
    }
}

} // namespace NCommon
