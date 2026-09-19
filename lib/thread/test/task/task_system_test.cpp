#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
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

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_open = false;
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
    NCommon::TaskSystem taskSystem{2};

    std::optional<NCommon::TaskHandle> observedTask;
    std::optional<NCommon::WorkerIndex> firstIndex;
    std::optional<NCommon::WorkerIndex> secondIndex;
    std::thread::id firstThread;
    std::thread::id secondThread;
    bool childHandleWasValid = false;

    const NCommon::TaskHandle task = taskSystem.Submit([&](NCommon::TaskContext& context) {
        observedTask = context.GetTask();
        firstIndex = context.GetWorkerIndex();
        firstThread = std::this_thread::get_id();

        const NCommon::TaskHandle child = context.Spawn([&](NCommon::TaskContext& childContext) {
            secondIndex = childContext.GetWorkerIndex();
            secondThread = std::this_thread::get_id();
        });

        childHandleWasValid = child.IsValid();
    });

    taskSystem.Wait(task);
    taskSystem.WaitIdle();

    ASSERT_TRUE(observedTask.has_value());
    ASSERT_TRUE(firstIndex.has_value());
    ASSERT_TRUE(secondIndex.has_value());
    EXPECT_TRUE(childHandleWasValid);
    EXPECT_EQ(*observedTask, task);
    EXPECT_LT(firstIndex->GetValue(), 2U);
    EXPECT_LT(secondIndex->GetValue(), 2U);

    if (firstThread == secondThread) {
        EXPECT_EQ(*firstIndex, *secondIndex);
    }
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
