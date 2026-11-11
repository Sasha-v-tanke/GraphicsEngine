#pragma once

#include <GraphicsEngine/lib/thread/task/task_system.h>

namespace NCommon {

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

} // namespace NCommon
