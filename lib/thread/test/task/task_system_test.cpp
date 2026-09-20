#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <lib/thread/task/task_system.h>

namespace {

using namespace std::chrono_literals;

class Gate final {
public:
    void Open() {
        {
            std::lock_guard lock{m_mutex};
            m_open = true;
        }

        m_condition.notify_all();
    }

    void Wait() {
        std::unique_lock lock{m_mutex};
        m_condition.wait(lock, [this] { return m_open; });
    }

    [[nodiscard]] bool WaitForOpen() {
        std::unique_lock lock{m_mutex};
        return m_condition.wait_for(lock, 2s, [this] { return m_open; });
    }

    [[nodiscard]] bool IsOpen() const {
        std::lock_guard lock{m_mutex};
        return m_open;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_open = false;
};

class CountedGate final {
public:
    void Arrive() {
        {
            std::lock_guard lock{m_mutex};
            ++m_count;
        }

        m_condition.notify_all();
    }

    [[nodiscard]] bool WaitForCount(std::size_t expected) {
        std::unique_lock lock{m_mutex};
        return m_condition.wait_for(lock, 2s, [this, expected] { return m_count >= expected; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::size_t m_count = 0;
};

TEST(TaskSystem, RunsTaskLifecycleToCompletion) {
    NCommon::TaskSystem taskSystem{1};
    Gate entered;
    Gate finish;

    bool executed = false;
    const NCommon::TaskHandle task = taskSystem.Submit([&](NCommon::TaskContext&) {
        entered.Open();
        finish.Wait();
        executed = true;
    });

    ASSERT_TRUE(entered.WaitForOpen());
    EXPECT_EQ(taskSystem.GetStatus(task), NCommon::ETaskStatus::RUNNING);

    finish.Open();
    taskSystem.Wait(task);

    EXPECT_TRUE(executed);
    EXPECT_EQ(taskSystem.GetStatus(task), NCommon::ETaskStatus::COMPLETED);
    EXPECT_FALSE(taskSystem.GetError(task).has_value());
}

TEST(TaskSystem, ProvidesTaskHandleAndStableWorkerIndexInContext) {
    constexpr std::size_t workerCount = 2;
    NCommon::TaskSystem taskSystem{workerCount};

    std::optional<NCommon::TaskHandle> observedTask;
    bool childHandleWasValid = false;
    std::mutex observationsMutex;
    std::map<std::thread::id, NCommon::WorkerIndex> workerIndices;

    const auto observeWorker = [&](NCommon::TaskContext& context) {
        std::lock_guard lock{observationsMutex};
        const std::thread::id threadId = std::this_thread::get_id();
        const NCommon::WorkerIndex workerIndex = context.GetWorkerIndex();
        const auto [it, inserted] = workerIndices.emplace(threadId, workerIndex);

        if (!inserted) {
            EXPECT_EQ(it->second, workerIndex);
        }

        EXPECT_LT(workerIndex.GetValue(), workerCount);
    };

    const NCommon::TaskHandle task = taskSystem.Submit([&](NCommon::TaskContext& context) {
        observedTask = context.GetTask();
        observeWorker(context);

        const NCommon::TaskHandle child =
                context.Spawn([&](NCommon::TaskContext& childContext) { observeWorker(childContext); });

        childHandleWasValid = child.IsValid();
    });

    taskSystem.Wait(task);
    taskSystem.WaitIdle();

    ASSERT_TRUE(observedTask.has_value());
    EXPECT_TRUE(childHandleWasValid);
    EXPECT_EQ(*observedTask, task);
    EXPECT_FALSE(workerIndices.empty());

    std::vector<NCommon::TaskHandle> tasks;
    tasks.reserve(16);

    for (std::size_t index = 0; index < 16; ++index) {
        tasks.push_back(taskSystem.Submit([&](NCommon::TaskContext& context) { observeWorker(context); }));
    }

    for (const NCommon::TaskHandle& handle: tasks) {
        taskSystem.Wait(handle);
    }

    taskSystem.WaitIdle();

    for (const auto& [threadId, workerIndex]: workerIndices) {
        static_cast<void>(threadId);
        EXPECT_LT(workerIndex.GetValue(), workerCount);
    }
}

TEST(TaskSystem, RunsIndependentReadyTasksConcurrentlyOnWorkers) {
    constexpr std::size_t workerCount = 4;
    CountedGate entered;
    Gate finish;

    std::atomic<std::size_t> enteredCount = 0;
    std::mutex observationsMutex;
    std::vector<std::thread::id> threadIds;
    std::vector<NCommon::WorkerIndex> workerIndices;
    bool allTasksEntered = false;

    {
        NCommon::TaskSystem taskSystem{workerCount};
        std::vector<NCommon::TaskHandle> tasks;
        tasks.reserve(workerCount);

        for (std::size_t index = 0; index < workerCount; ++index) {
            tasks.push_back(taskSystem.Submit([&](NCommon::TaskContext& context) {
                enteredCount.fetch_add(1);

                {
                    std::lock_guard lock{observationsMutex};
                    threadIds.push_back(std::this_thread::get_id());
                    workerIndices.push_back(context.GetWorkerIndex());
                }

                entered.Arrive();
                finish.Wait();
            }));
        }

        allTasksEntered = entered.WaitForCount(workerCount);
        finish.Open();

        if (allTasksEntered) {
            for (const NCommon::TaskHandle& task: tasks) {
                taskSystem.Wait(task);
            }
        }
    }

    ASSERT_TRUE(allTasksEntered);
    EXPECT_EQ(enteredCount.load(), workerCount);
    ASSERT_EQ(threadIds.size(), workerCount);
    ASSERT_EQ(workerIndices.size(), workerCount);

    for (const std::thread::id threadId: threadIds) {
        EXPECT_NE(threadId, std::this_thread::get_id());
    }

    for (const NCommon::WorkerIndex workerIndex: workerIndices) {
        EXPECT_LT(workerIndex.GetValue(), workerCount);
    }
}

TEST(TaskSystem, IdleWorkerSleepsAndWakesForSubmittedTask) {
    const std::thread::id applicationThread = std::this_thread::get_id();
    std::thread::id executionThread;
    NCommon::WorkerIndex workerIndex{2};
    Gate taskRan;
    bool taskStarted = false;

    {
        NCommon::TaskSystem taskSystem{2};
        EXPECT_EQ(taskSystem.GetWorkerCount(), 2U);
        taskSystem.WaitIdle();

        const NCommon::TaskHandle task = taskSystem.Submit([&](NCommon::TaskContext& context) {
            workerIndex = context.GetWorkerIndex();
            executionThread = std::this_thread::get_id();
            taskRan.Open();
        });

        taskStarted = taskRan.WaitForOpen();

        if (taskStarted) {
            taskSystem.Wait(task);
        }
    }

    ASSERT_TRUE(taskStarted);
    EXPECT_NE(executionThread, applicationThread);
    EXPECT_LT(workerIndex.GetValue(), 2U);
}

TEST(TaskSystem, RepeatedStartStopKeepsWorkerCountAndExecutesWork) {
    constexpr std::size_t workerCount = 3;

    for (std::size_t iteration = 0; iteration < 16; ++iteration) {
        NCommon::TaskSystem taskSystem{workerCount};
        EXPECT_EQ(taskSystem.GetWorkerCount(), workerCount);

        std::atomic<std::size_t> completed = 0;
        std::mutex observationsMutex;
        std::vector<NCommon::WorkerIndex> workerIndices;
        std::vector<NCommon::TaskHandle> tasks;
        tasks.reserve(workerCount * 2);
        workerIndices.reserve(workerCount * 2);

        for (std::size_t index = 0; index < workerCount * 2; ++index) {
            tasks.push_back(taskSystem.Submit([&](NCommon::TaskContext& context) {
                {
                    std::lock_guard lock{observationsMutex};
                    workerIndices.push_back(context.GetWorkerIndex());
                }

                completed.fetch_add(1);
            }));
        }

        for (const NCommon::TaskHandle& task: tasks) {
            taskSystem.Wait(task);
        }

        EXPECT_EQ(completed.load(), workerCount * 2);

        ASSERT_EQ(workerIndices.size(), workerCount * 2);
        for (const NCommon::WorkerIndex workerIndex: workerIndices) {
            EXPECT_LT(workerIndex.GetValue(), workerCount);
        }
    }
}

TEST(TaskSystem, NormalizesZeroWorkerCountToOne) {
    NCommon::TaskSystem taskSystem{0};
    EXPECT_EQ(taskSystem.GetWorkerCount(), 1U);

    NCommon::WorkerIndex workerIndex{taskSystem.GetWorkerCount()};
    const NCommon::TaskHandle task =
            taskSystem.Submit([&](NCommon::TaskContext& context) { workerIndex = context.GetWorkerIndex(); });

    taskSystem.Wait(task);

    EXPECT_EQ(workerIndex.GetValue(), 0U);
}

TEST(TaskSystem, RejectsInvalidAndForeignHandles) {
    NCommon::TaskSystem firstSystem{1};
    NCommon::TaskSystem secondSystem{1};

    const NCommon::TaskHandle firstTask = firstSystem.Submit([](NCommon::TaskContext&) {});
    const NCommon::TaskHandle secondTask = secondSystem.Submit([](NCommon::TaskContext&) {});

    firstSystem.Wait(firstTask);
    secondSystem.Wait(secondTask);

    EXPECT_THROW({ static_cast<void>(firstSystem.GetStatus(NCommon::TaskHandle{})); }, NCommon::Exception);
    EXPECT_THROW({ static_cast<void>(firstSystem.GetStatus(secondTask)); }, NCommon::Exception);
    EXPECT_THROW(firstSystem.Cancel(secondTask), NCommon::Exception);

    const std::vector<NCommon::TaskHandle> dependencies{secondTask};
    EXPECT_THROW(firstSystem.Submit([](NCommon::TaskContext&) {}, dependencies), NCommon::Exception);
}

TEST(TaskSystem, RejectsHandleFromDestroyedSystemRecreatedInSameStorage) {
    alignas(NCommon::TaskSystem) std::byte storage[sizeof(NCommon::TaskSystem)];

    auto* firstSystem = new (&storage) NCommon::TaskSystem{1};
    const NCommon::TaskHandle staleTask = firstSystem->Submit([](NCommon::TaskContext&) {});
    firstSystem->Wait(staleTask);
    firstSystem->~TaskSystem();

    auto* secondSystem = new (&storage) NCommon::TaskSystem{1};
    const NCommon::TaskHandle secondTask = secondSystem->Submit([](NCommon::TaskContext&) {});
    secondSystem->Wait(secondTask);

    EXPECT_THROW({ static_cast<void>(secondSystem->GetStatus(staleTask)); }, NCommon::Exception);

    secondSystem->~TaskSystem();
}

TEST(TaskSystem, RejectsDuplicateDependency) {
    NCommon::TaskSystem taskSystem{1};

    const NCommon::TaskHandle dependency = taskSystem.Submit([](NCommon::TaskContext&) {});
    const std::vector<NCommon::TaskHandle> dependencies{dependency, dependency};

    EXPECT_THROW(taskSystem.Submit([](NCommon::TaskContext&) {}, dependencies), NCommon::Exception);

    taskSystem.Wait(dependency);
}

TEST(TaskSystem, RejectsCancellingRunningTask) {
    NCommon::TaskSystem taskSystem{1};
    Gate entered;
    Gate finish;

    const NCommon::TaskHandle task = taskSystem.Submit([&](NCommon::TaskContext&) {
        entered.Open();
        finish.Wait();
    });

    ASSERT_TRUE(entered.WaitForOpen());
    EXPECT_EQ(taskSystem.GetStatus(task), NCommon::ETaskStatus::RUNNING);
    EXPECT_THROW(taskSystem.Cancel(task), NCommon::Exception);

    finish.Open();
    taskSystem.Wait(task);
}

TEST(TaskSystem, RunsDependentTaskAfterMultiplePrerequisitesComplete) {
    NCommon::TaskSystem taskSystem{2};
    Gate finishFirst;
    Gate finishSecond;

    std::vector<int> order;
    std::mutex orderMutex;

    const NCommon::TaskHandle first = taskSystem.Submit([&](NCommon::TaskContext&) {
        finishFirst.Wait();
        std::lock_guard lock{orderMutex};
        order.push_back(1);
    });
    const NCommon::TaskHandle second = taskSystem.Submit([&](NCommon::TaskContext&) {
        finishSecond.Wait();
        std::lock_guard lock{orderMutex};
        order.push_back(2);
    });

    const std::vector<NCommon::TaskHandle> dependencies{first, second};
    const NCommon::TaskHandle dependent = taskSystem.Submit(
            [&](NCommon::TaskContext&) {
                std::lock_guard lock{orderMutex};
                order.push_back(3);
            },
            dependencies);

    EXPECT_EQ(taskSystem.GetStatus(dependent), NCommon::ETaskStatus::WAITING);

    finishFirst.Open();
    taskSystem.Wait(first);
    EXPECT_EQ(taskSystem.GetStatus(dependent), NCommon::ETaskStatus::WAITING);

    finishSecond.Open();
    taskSystem.Wait(dependent);

    EXPECT_EQ(order.back(), 3);
}

TEST(TaskSystem, RunsDependencyChainsInOrder) {
    NCommon::TaskSystem taskSystem{3};

    std::atomic<int> completedStage = 0;
    NCommon::TaskHandle previous = taskSystem.Submit([&](NCommon::TaskContext&) { completedStage.store(1); });

    for (int stage = 2; stage <= 8; ++stage) {
        const std::vector<NCommon::TaskHandle> dependencies{previous};
        previous = taskSystem.Submit(
                [&, stage](NCommon::TaskContext&) {
                    EXPECT_EQ(completedStage.load(), stage - 1);
                    completedStage.store(stage);
                },
                dependencies);
    }

    taskSystem.Wait(previous);

    EXPECT_EQ(completedStage.load(), 8);
}

TEST(TaskSystem, RunsFanOutDependentsAfterSharedDependencyCompletes) {
    NCommon::TaskSystem taskSystem{4};
    Gate dependencyFinish;

    const NCommon::TaskHandle dependency = taskSystem.Submit([&](NCommon::TaskContext&) { dependencyFinish.Wait(); });
    const std::vector<NCommon::TaskHandle> dependencies{dependency};

    std::atomic<std::size_t> dependentsCompleted = 0;
    std::vector<NCommon::TaskHandle> dependents;
    dependents.reserve(12);

    for (std::size_t index = 0; index < 12; ++index) {
        dependents.push_back(taskSystem.Submit(
                [&](NCommon::TaskContext&) {
                    EXPECT_EQ(taskSystem.GetStatus(dependency), NCommon::ETaskStatus::COMPLETED);
                    dependentsCompleted.fetch_add(1);
                },
                dependencies));
    }

    for (const NCommon::TaskHandle& dependent: dependents) {
        EXPECT_EQ(taskSystem.GetStatus(dependent), NCommon::ETaskStatus::WAITING);
    }

    dependencyFinish.Open();

    for (const NCommon::TaskHandle& dependent: dependents) {
        taskSystem.Wait(dependent);
    }

    EXPECT_EQ(dependentsCompleted.load(), dependents.size());
}

TEST(TaskSystem, RunningTaskPublishesDynamicChildWithDependencies) {
    NCommon::TaskSystem taskSystem{2};
    Gate dependencyFinish;

    const NCommon::TaskHandle dependency = taskSystem.Submit([&](NCommon::TaskContext&) { dependencyFinish.Wait(); });
    std::optional<NCommon::TaskHandle> child;
    bool childSawDependency = false;

    const NCommon::TaskHandle parent = taskSystem.Submit([&](NCommon::TaskContext& context) {
        const std::vector<NCommon::TaskHandle> dependencies{dependency};
        child = context.Spawn(
                [&](NCommon::TaskContext&) {
                    childSawDependency = taskSystem.GetStatus(dependency) == NCommon::ETaskStatus::COMPLETED;
                },
                dependencies);
    });

    taskSystem.Wait(parent);
    ASSERT_TRUE(child.has_value());
    EXPECT_EQ(taskSystem.GetStatus(*child), NCommon::ETaskStatus::WAITING);

    dependencyFinish.Open();
    taskSystem.Wait(*child);

    EXPECT_TRUE(childSawDependency);
}

TEST(TaskSystem, HandlesConcurrentDependencyCompletionAndDependentPublication) {
    constexpr std::size_t iterations = 128;

    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        NCommon::TaskSystem taskSystem{2};
        Gate dependencyFinish;

        const NCommon::TaskHandle dependency =
                taskSystem.Submit([&](NCommon::TaskContext&) { dependencyFinish.Wait(); });

        std::optional<NCommon::TaskHandle> dependent;
        std::thread publisher{[&] {
            const std::vector<NCommon::TaskHandle> dependencies{dependency};
            dependent = taskSystem.Submit([](NCommon::TaskContext&) {}, dependencies);
        }};

        dependencyFinish.Open();
        publisher.join();

        ASSERT_TRUE(dependent.has_value());
        taskSystem.Wait(*dependent);
        EXPECT_EQ(taskSystem.GetStatus(*dependent), NCommon::ETaskStatus::COMPLETED);
    }
}

TEST(TaskSystem, CancelsReadyAndWaitingTasks) {
    NCommon::TaskSystem taskSystem{1};
    Gate blockerFinish;

    const NCommon::TaskHandle blocker = taskSystem.Submit([&](NCommon::TaskContext&) { blockerFinish.Wait(); });
    const NCommon::TaskHandle ready = taskSystem.Submit([](NCommon::TaskContext&) {});
    const std::vector<NCommon::TaskHandle> dependencies{blocker};
    const NCommon::TaskHandle waiting = taskSystem.Submit([](NCommon::TaskContext&) {}, dependencies);

    EXPECT_EQ(taskSystem.GetStatus(ready), NCommon::ETaskStatus::READY);
    EXPECT_EQ(taskSystem.GetStatus(waiting), NCommon::ETaskStatus::WAITING);

    taskSystem.Cancel(ready);
    taskSystem.Cancel(waiting);

    EXPECT_EQ(taskSystem.GetStatus(ready), NCommon::ETaskStatus::CANCELLED);
    EXPECT_EQ(taskSystem.GetStatus(waiting), NCommon::ETaskStatus::CANCELLED);

    blockerFinish.Open();
    taskSystem.Wait(blocker);
    taskSystem.WaitIdle();
}

TEST(TaskSystem, WaitIdleCompletesAfterCancelledReadyTaskLeavesOnlyQueueTombstone) {
    NCommon::TaskSystem taskSystem{1};
    Gate blockerStarted;
    Gate blockerFinish;
    Gate waitIdleStarted;
    Gate waitIdleFinished;

    const NCommon::TaskHandle blocker = taskSystem.Submit([&](NCommon::TaskContext&) {
        blockerStarted.Open();
        blockerFinish.Wait();
    });
    const NCommon::TaskHandle ready = taskSystem.Submit([](NCommon::TaskContext&) {});

    ASSERT_TRUE(blockerStarted.WaitForOpen());
    EXPECT_EQ(taskSystem.GetStatus(ready), NCommon::ETaskStatus::READY);

    std::thread waiter{[&] {
        waitIdleStarted.Open();
        taskSystem.WaitIdle();
        waitIdleFinished.Open();
    }};

    ASSERT_TRUE(waitIdleStarted.WaitForOpen());
    taskSystem.Cancel(ready);
    EXPECT_EQ(taskSystem.GetStatus(ready), NCommon::ETaskStatus::CANCELLED);
    EXPECT_FALSE(waitIdleFinished.IsOpen());

    blockerFinish.Open();

    EXPECT_TRUE(waitIdleFinished.WaitForOpen());
    waiter.join();
    taskSystem.Wait(blocker);
}

TEST(TaskSystem, CopiesDependencySetWhenTaskIsPublished) {
    NCommon::TaskSystem taskSystem{1};

    bool firstDone = false;
    bool secondRan = false;
    bool thirdRan = false;

    const NCommon::TaskHandle first = taskSystem.Submit([&](NCommon::TaskContext&) { firstDone = true; });
    std::vector<NCommon::TaskHandle> dependencies{first};

    const NCommon::TaskHandle second = taskSystem.Submit(
            [&](NCommon::TaskContext&) {
                if (firstDone) {
                    secondRan = true;
                }
            },
            dependencies);

    const NCommon::TaskHandle third = taskSystem.Submit([&](NCommon::TaskContext&) { thirdRan = true; });
    dependencies.push_back(third);

    taskSystem.Wait(second);
    taskSystem.Wait(third);

    EXPECT_TRUE(secondRan);
    EXPECT_TRUE(thirdRan);
}

TEST(TaskSystem, CapturesStandardAndProjectExceptionsAsTaskFailures) {
    NCommon::TaskSystem taskSystem{1};

    const NCommon::TaskHandle standardTask =
            taskSystem.Submit([](NCommon::TaskContext&) { throw std::runtime_error{"boom"}; });
    const NCommon::TaskHandle projectTask = taskSystem.Submit(
            [](NCommon::TaskContext&) { GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "bad state"); });

    taskSystem.Wait(standardTask);
    taskSystem.Wait(projectTask);

    const std::optional<NCommon::ErrorInfo> standardError = taskSystem.GetError(standardTask);
    const std::optional<NCommon::ErrorInfo> projectError = taskSystem.GetError(projectTask);

    ASSERT_TRUE(standardError.has_value());
    ASSERT_TRUE(projectError.has_value());
    EXPECT_EQ(taskSystem.GetStatus(standardTask), NCommon::ETaskStatus::FAILED);
    EXPECT_EQ(standardError->Code, NCommon::make_error_code(NCommon::EError::UNKNOWN));
    EXPECT_NE(std::string_view{standardError->Message}.find("boom"), std::string_view::npos);
    EXPECT_EQ(projectError->Code, NCommon::make_error_code(NCommon::EError::INVALID_STATE));
    EXPECT_EQ(projectError->Message, "bad state");
}

TEST(TaskSystem, CapturesSystemAndUnknownExceptionsAsTaskFailures) {
    NCommon::TaskSystem taskSystem{1};

    const auto ioError = std::make_error_code(std::errc::io_error);
    const NCommon::TaskHandle systemTask =
            taskSystem.Submit([&](NCommon::TaskContext&) { throw std::system_error{ioError, "io"}; });
    const NCommon::TaskHandle unknownTask = taskSystem.Submit([](NCommon::TaskContext&) { throw 7; });

    taskSystem.Wait(systemTask);
    taskSystem.Wait(unknownTask);

    const std::optional<NCommon::ErrorInfo> systemError = taskSystem.GetError(systemTask);
    const std::optional<NCommon::ErrorInfo> unknownError = taskSystem.GetError(unknownTask);

    ASSERT_TRUE(systemError.has_value());
    ASSERT_TRUE(unknownError.has_value());
    EXPECT_EQ(systemError->Code, ioError);
    EXPECT_EQ(unknownError->Code, NCommon::make_error_code(NCommon::EError::UNKNOWN));
}

TEST(TaskSystem, CancelsMultipleDependentsWhenDependencyFails) {
    NCommon::TaskSystem taskSystem{1};

    const NCommon::TaskHandle first =
            taskSystem.Submit([](NCommon::TaskContext&) { throw std::runtime_error{"boom"}; });

    const std::vector<NCommon::TaskHandle> dependencies{first};
    const NCommon::TaskHandle second = taskSystem.Submit([](NCommon::TaskContext&) {}, dependencies);
    const NCommon::TaskHandle third = taskSystem.Submit([](NCommon::TaskContext&) {}, dependencies);

    taskSystem.Wait(second);
    taskSystem.Wait(third);

    EXPECT_EQ(taskSystem.GetStatus(first), NCommon::ETaskStatus::FAILED);
    EXPECT_EQ(taskSystem.GetStatus(second), NCommon::ETaskStatus::CANCELLED);
    EXPECT_EQ(taskSystem.GetStatus(third), NCommon::ETaskStatus::CANCELLED);
}

TEST(TaskSystem, CancelsFailedDependencyTree) {
    NCommon::TaskSystem taskSystem{2};

    const NCommon::TaskHandle root = taskSystem.Submit(
            [](NCommon::TaskContext&) { GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "root"); });

    const std::vector<NCommon::TaskHandle> rootDependencies{root};
    const NCommon::TaskHandle left = taskSystem.Submit([](NCommon::TaskContext&) {}, rootDependencies);
    const NCommon::TaskHandle right = taskSystem.Submit([](NCommon::TaskContext&) {}, rootDependencies);

    const std::vector<NCommon::TaskHandle> childDependencies{left, right};
    const NCommon::TaskHandle leaf = taskSystem.Submit([](NCommon::TaskContext&) {}, childDependencies);

    taskSystem.Wait(leaf);

    EXPECT_EQ(taskSystem.GetStatus(root), NCommon::ETaskStatus::FAILED);
    EXPECT_EQ(taskSystem.GetStatus(left), NCommon::ETaskStatus::CANCELLED);
    EXPECT_EQ(taskSystem.GetStatus(right), NCommon::ETaskStatus::CANCELLED);
    EXPECT_EQ(taskSystem.GetStatus(leaf), NCommon::ETaskStatus::CANCELLED);
}

TEST(TaskSystem, CancelsDependentsWhenDependencyIsCancelled) {
    NCommon::TaskSystem taskSystem{1};
    Gate blockerFinish;

    const NCommon::TaskHandle blocker = taskSystem.Submit([&](NCommon::TaskContext&) { blockerFinish.Wait(); });
    const std::vector<NCommon::TaskHandle> blockerDependencies{blocker};
    const NCommon::TaskHandle dependency = taskSystem.Submit([](NCommon::TaskContext&) {}, blockerDependencies);
    const std::vector<NCommon::TaskHandle> dependencies{dependency};
    const NCommon::TaskHandle dependent = taskSystem.Submit([](NCommon::TaskContext&) {}, dependencies);

    taskSystem.Cancel(dependency);
    blockerFinish.Open();

    taskSystem.Wait(dependent);

    EXPECT_EQ(taskSystem.GetStatus(dependency), NCommon::ETaskStatus::CANCELLED);
    EXPECT_EQ(taskSystem.GetStatus(dependent), NCommon::ETaskStatus::CANCELLED);
}

TEST(TaskSystem, ShutdownCancelsPendingWorkAndLetsRunningTasksFinish) {
    auto* taskSystem = new NCommon::TaskSystem{2};

    CountedGate runningEntered;
    Gate finishRunning;
    Gate deleteStarted;
    std::atomic<std::size_t> runningFinished = 0;
    std::atomic<std::size_t> queuedRan = 0;
    std::atomic<bool> stoppingRejectedSubmission = false;

    for (std::size_t index = 0; index < 2; ++index) {
        taskSystem->Submit([&, index](NCommon::TaskContext& context) {
            runningEntered.Arrive();
            finishRunning.Wait();

            if (index == 0) {
                while (!stoppingRejectedSubmission.load()) {
                    try {
                        context.Spawn([](NCommon::TaskContext&) {});
                        std::this_thread::yield();
                    } catch (const NCommon::Exception&) {
                        stoppingRejectedSubmission = true;
                    }
                }
            }

            runningFinished.fetch_add(1);
        });
    }

    ASSERT_TRUE(runningEntered.WaitForCount(2));

    for (std::size_t index = 0; index < 32; ++index) {
        taskSystem->Submit([&](NCommon::TaskContext&) { queuedRan.fetch_add(1); });
    }

    std::thread destroyer{[&] {
        deleteStarted.Open();
        delete taskSystem;
    }};

    ASSERT_TRUE(deleteStarted.WaitForOpen());
    finishRunning.Open();
    destroyer.join();

    EXPECT_EQ(runningFinished.load(), 2U);
    EXPECT_EQ(queuedRan.load(), 0U);
    EXPECT_TRUE(stoppingRejectedSubmission.load());
}

TEST(TaskSystem, ReleasesCallablePayloadAfterTerminalState) {
    NCommon::TaskSystem taskSystem{1};
    std::weak_ptr<int> weakPayload;

    NCommon::TaskHandle task;
    {
        auto payload = std::make_shared<int>(42);
        weakPayload = payload;
        task = taskSystem.Submit([payload](NCommon::TaskContext&) { static_cast<void>(*payload); });
    }

    taskSystem.Wait(task);
    EXPECT_TRUE(weakPayload.expired());
}

TEST(TaskSystem, RunsManySubmitWaitCyclesWithoutRetainingPayloads) {
    NCommon::TaskSystem taskSystem{1};

    for (int index = 0; index < 64; ++index) {
        std::weak_ptr<int> weakPayload;

        {
            auto payload = std::make_shared<int>(index);
            weakPayload = payload;
            const NCommon::TaskHandle task =
                    taskSystem.Submit([payload](NCommon::TaskContext&) { static_cast<void>(*payload); });
            taskSystem.Wait(task);
        }

        EXPECT_TRUE(weakPayload.expired());
    }
}

TEST(TaskSystem, RejectsWaitFromWorkerThread) {
    NCommon::TaskSystem taskSystem{1};

    bool waitRejected = false;

    const NCommon::TaskHandle parent = taskSystem.Submit([&taskSystem, &waitRejected](NCommon::TaskContext& context) {
        const NCommon::TaskHandle child = context.Spawn([](NCommon::TaskContext&) {});

        try {
            taskSystem.Wait(child);
        } catch (const NCommon::Exception&) {
            waitRejected = true;
        }
    });

    taskSystem.Wait(parent);
    taskSystem.WaitIdle();

    EXPECT_TRUE(waitRejected);
}

TEST(TaskSystem, RejectsWaitIdleFromWorkerThread) {
    NCommon::TaskSystem taskSystem{1};

    bool waitRejected = false;

    const NCommon::TaskHandle task = taskSystem.Submit([&taskSystem, &waitRejected](NCommon::TaskContext&) {
        try {
            taskSystem.WaitIdle();
        } catch (const NCommon::Exception&) {
            waitRejected = true;
        }
    });

    taskSystem.Wait(task);

    EXPECT_TRUE(waitRejected);
}

} // namespace
