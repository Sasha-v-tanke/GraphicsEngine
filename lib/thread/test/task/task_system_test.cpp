#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <lib/thread/task/task_system.h>

namespace {

TEST(TaskSystem, RunsTaskLifecycleToCompletion) {
    NCommon::TaskSystem taskSystem{1};

    std::atomic_bool executed = false;

    const NCommon::TaskHandle task = taskSystem.Submit([&executed](NCommon::TaskContext&) { executed = true; });

    taskSystem.Wait(task);

    EXPECT_TRUE(executed);
    EXPECT_EQ(taskSystem.GetStatus(task), NCommon::ETaskStatus::COMPLETED);
}

TEST(TaskSystem, ProvidesTaskHandleAndWorkerIndexInContext) {
    NCommon::TaskSystem taskSystem{2};

    std::atomic_bool verified = false;

    const NCommon::TaskHandle blocker = taskSystem.Submit([](NCommon::TaskContext&) {});
    const std::vector<NCommon::TaskHandle> dependencies{blocker};

    NCommon::TaskHandle task;
    task = taskSystem.Submit(
            [&task, &verified](NCommon::TaskContext& context) {
                EXPECT_EQ(context.GetTask(), task);
                EXPECT_LT(context.GetWorkerIndex().GetValue(), 2U);

                verified = true;
            },
            dependencies);

    taskSystem.Wait(task);

    EXPECT_TRUE(verified);
}

TEST(TaskSystem, RejectsInvalidHandles) {
    NCommon::TaskSystem taskSystem{1};

    EXPECT_THROW({ static_cast<void>(taskSystem.GetStatus(NCommon::TaskHandle{})); }, NCommon::Exception);
}

TEST(TaskSystem, RejectsCancellingRunningTask) {
    NCommon::TaskSystem taskSystem{1};

    std::atomic_bool canFinish = false;

    const NCommon::TaskHandle task = taskSystem.Submit([&canFinish](NCommon::TaskContext&) {
        while (!canFinish) {
            std::this_thread::yield();
        }
    });

    while (taskSystem.GetStatus(task) != NCommon::ETaskStatus::RUNNING) {
        std::this_thread::yield();
    }

    EXPECT_THROW(taskSystem.Cancel(task), NCommon::Exception);

    canFinish = true;
    taskSystem.Wait(task);
}

TEST(TaskSystem, RunsDependentTaskAfterDependencyCompletes) {
    NCommon::TaskSystem taskSystem{1};

    std::vector<int> order;

    const NCommon::TaskHandle first = taskSystem.Submit([&order](NCommon::TaskContext&) { order.push_back(1); });

    const std::vector<NCommon::TaskHandle> dependencies{first};

    const NCommon::TaskHandle second =
            taskSystem.Submit([&order](NCommon::TaskContext&) { order.push_back(2); }, dependencies);

    taskSystem.Wait(second);

    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(TaskSystem, CopiesDependencySetWhenTaskIsPublished) {
    NCommon::TaskSystem taskSystem{1};

    std::atomic_bool firstDone = false;
    std::atomic_bool secondRan = false;
    std::atomic_bool thirdRan = false;

    const NCommon::TaskHandle first = taskSystem.Submit([&firstDone](NCommon::TaskContext&) { firstDone = true; });

    std::vector<NCommon::TaskHandle> dependencies{first};

    const NCommon::TaskHandle second = taskSystem.Submit(
            [&firstDone, &secondRan](NCommon::TaskContext&) {
                EXPECT_TRUE(firstDone);
                secondRan = true;
            },
            dependencies);

    const NCommon::TaskHandle third = taskSystem.Submit([&thirdRan](NCommon::TaskContext&) { thirdRan = true; });

    dependencies.push_back(third);

    taskSystem.Wait(second);
    taskSystem.Wait(third);

    EXPECT_TRUE(secondRan);
    EXPECT_TRUE(thirdRan);
}

TEST(TaskSystem, RunningTaskCanSpawnTasks) {
    NCommon::TaskSystem taskSystem{2};

    std::atomic_bool childRan = false;

    const NCommon::TaskHandle parent = taskSystem.Submit([&childRan](NCommon::TaskContext& context) {
        const NCommon::TaskHandle child = context.Spawn([&childRan](NCommon::TaskContext&) { childRan = true; });

        EXPECT_TRUE(child.IsValid());
    });

    taskSystem.Wait(parent);
    taskSystem.WaitIdle();

    EXPECT_TRUE(childRan);
}

TEST(TaskSystem, CapturesExceptionAsTaskFailure) {
    NCommon::TaskSystem taskSystem{1};

    const NCommon::TaskHandle task = taskSystem.Submit([](NCommon::TaskContext&) { throw std::runtime_error{"boom"}; });

    taskSystem.Wait(task);

    EXPECT_EQ(taskSystem.GetStatus(task), NCommon::ETaskStatus::FAILED);
    EXPECT_NE(taskSystem.GetError(task), nullptr);
}

TEST(TaskSystem, CancelsDependentTaskWhenDependencyFails) {
    NCommon::TaskSystem taskSystem{1};

    const NCommon::TaskHandle first =
            taskSystem.Submit([](NCommon::TaskContext&) { throw std::runtime_error{"boom"}; });

    const std::vector<NCommon::TaskHandle> dependencies{first};

    const NCommon::TaskHandle second = taskSystem.Submit([](NCommon::TaskContext&) {}, dependencies);

    taskSystem.Wait(second);

    EXPECT_EQ(taskSystem.GetStatus(first), NCommon::ETaskStatus::FAILED);
    EXPECT_EQ(taskSystem.GetStatus(second), NCommon::ETaskStatus::CANCELLED);
}

TEST(TaskSystem, InvalidDependencyDoesNotCorruptGraph) {
    NCommon::TaskSystem taskSystem{1};

    const NCommon::TaskHandle dependency =
            taskSystem.Submit([](NCommon::TaskContext&) {});

    const std::vector<NCommon::TaskHandle> dependencies{
        dependency,
        NCommon::TaskHandle{},
    };

    EXPECT_THROW(
            taskSystem.Submit([](NCommon::TaskContext&) {}, dependencies),
            NCommon::Exception);

    taskSystem.Wait(dependency);
    taskSystem.WaitIdle();

    EXPECT_EQ(taskSystem.GetStatus(dependency), NCommon::ETaskStatus::COMPLETED);
}

TEST(TaskSystem, RejectsWaitFromWorkerThread) {
    NCommon::TaskSystem taskSystem{1};

    std::atomic_bool waitRejected = false;

    const NCommon::TaskHandle parent =
            taskSystem.Submit([&taskSystem, &waitRejected](NCommon::TaskContext& context) {
                const NCommon::TaskHandle child =
                        context.Spawn([](NCommon::TaskContext&) {});

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

    std::atomic_bool waitRejected = false;

    const NCommon::TaskHandle task =
            taskSystem.Submit([&taskSystem, &waitRejected](NCommon::TaskContext&) {
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
