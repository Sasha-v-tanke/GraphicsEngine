#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <lib/thread/task/task_system.h>

namespace {

TEST(TaskGroup, JoinsDeepDescendantsAfterSealOnOneWorker) {
    NCommon::TaskSystem pool{1};
    const NCommon::TaskGroupHandle group = pool.CreateTaskGroup();
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    std::atomic_size_t visited{0};
    std::function<void(NCommon::TaskContext&, std::size_t)> recurse;
    recurse = [&](NCommon::TaskContext& context, std::size_t depth) {
        ++visited;
        if (depth != 0) {
            context.SpawnInGroup([&, depth](NCommon::TaskContext& child) { recurse(child, depth - 1); });
        }
    };
    pool.SubmitToGroup(group, [&](NCommon::TaskContext& context) {
        entered.release();
        resume.acquire();
        recurse(context, 150);
    });
    entered.acquire();
    std::atomic_size_t observed{0};
    const NCommon::TaskHandle early =
            pool.SubmitAfterGroup(group, [&](NCommon::TaskContext&) { observed.store(visited.load()); });
    pool.SealGroup(group);
    EXPECT_THROW(pool.SubmitToGroup(group, [](NCommon::TaskContext&) {}), NCommon::Exception);
    EXPECT_EQ(pool.GetGroupStatus(group), NCommon::ETaskGroupStatus::SEALED);
    resume.release();
    pool.WaitGroup(group);
    pool.Wait(early);
    EXPECT_EQ(observed.load(), 151U);
    EXPECT_EQ(pool.GetGroupStatus(group), NCommon::ETaskGroupStatus::COMPLETED);
    const NCommon::TaskHandle late = pool.SubmitAfterGroup(group, [](NCommon::TaskContext&) {});
    pool.Wait(late);
    EXPECT_EQ(pool.GetStatus(late), NCommon::ETaskStatus::COMPLETED);
    pool.WaitIdle();
}

TEST(TaskGroup, SpawnStormAndConcurrentContinuations) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        NCommon::TaskSystem pool{workers};
        const NCommon::TaskGroupHandle group = pool.CreateTaskGroup();
        std::atomic_size_t count{0};
        for (std::size_t root = 0; root < 24; ++root) {
            pool.SubmitToGroup(group, [&](NCommon::TaskContext& context) {
                for (std::size_t child = 0; child < 24; ++child) {
                    context.SpawnInGroup([&](NCommon::TaskContext&) { ++count; });
                }
            });
        }
        std::atomic_size_t continuations{0};
        std::vector<std::thread> producers;
        producers.reserve(4);
        for (std::size_t index = 0; index < 4; ++index) {
            producers.emplace_back([&] {
                for (std::size_t continuation = 0; continuation < 24; ++continuation) {
                    pool.SubmitAfterGroup(group, [&](NCommon::TaskContext&) {
                        EXPECT_EQ(count.load(), 576U);
                        ++continuations;
                    });
                }
            });
        }
        pool.SealGroup(group);
        for (std::thread& producer: producers) {
            producer.join();
        }
        pool.WaitGroup(group);
        pool.WaitIdle();
        EXPECT_EQ(continuations.load(), 96U);
    }
}

TEST(TaskGroup, FailureCancelsQueuedWorkAndPreservesFirstError) {
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    NCommon::TaskSystem pool{1};
    const NCommon::TaskGroupHandle group = pool.CreateTaskGroup();
    pool.SubmitToGroup(group, [&](NCommon::TaskContext&) {
        entered.release();
        resume.acquire();
        throw std::runtime_error("first group error");
    });
    entered.acquire();
    std::atomic_size_t invoked{0};
    const NCommon::TaskHandle queued = pool.SubmitToGroup(group, [&](NCommon::TaskContext&) { ++invoked; });
    const NCommon::TaskHandle continuation = pool.SubmitAfterGroup(group, [&](NCommon::TaskContext&) { ++invoked; });
    pool.SealGroup(group);
    resume.release();
    pool.WaitGroup(group);
    pool.WaitIdle();
    EXPECT_EQ(pool.GetGroupStatus(group), NCommon::ETaskGroupStatus::FAILED);
    ASSERT_TRUE(pool.GetGroupError(group).has_value());
    EXPECT_EQ(pool.GetGroupError(group)->Message, "first group error");
    EXPECT_EQ(pool.GetStatus(queued), NCommon::ETaskStatus::CANCELLED);
    EXPECT_EQ(pool.GetStatus(continuation), NCommon::ETaskStatus::CANCELLED);
    EXPECT_EQ(invoked.load(), 0U);
    const NCommon::TaskHandle late = pool.SubmitAfterGroup(group, [](NCommon::TaskContext&) {});
    pool.Wait(late);
    EXPECT_EQ(pool.GetStatus(late), NCommon::ETaskStatus::CANCELLED);
}

TEST(TaskGroup, CancellationDrainsRunningMembersAndRejectsSpawn) {
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    NCommon::TaskSystem pool{2};
    const NCommon::TaskGroupHandle group = pool.CreateTaskGroup();
    pool.SubmitToGroup(group, [&](NCommon::TaskContext& context) {
        entered.release();
        resume.acquire();
        EXPECT_TRUE(context.IsGroupCancellationRequested());
        EXPECT_THROW(context.SpawnInGroup([](NCommon::TaskContext&) {}), NCommon::Exception);
        EXPECT_THROW(pool.WaitGroup(group), NCommon::Exception);
    });
    entered.acquire();
    pool.CancelGroup(group);
    pool.CancelGroup(group);
    EXPECT_EQ(pool.GetGroupStatus(group), NCommon::ETaskGroupStatus::SEALED);
    resume.release();
    pool.WaitGroup(group);
    EXPECT_EQ(pool.GetGroupStatus(group), NCommon::ETaskGroupStatus::CANCELLED);
    EXPECT_FALSE(pool.GetGroupError(group).has_value());
    pool.WaitIdle();
}

TEST(TaskGroup, DirectMemberCancellationRetiresCapturesAndCancelsGroup) {
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    NCommon::TaskSystem pool{1};
    pool.Submit([&](NCommon::TaskContext&) {
        entered.release();
        resume.acquire();
    });
    entered.acquire();
    const NCommon::TaskGroupHandle group = pool.CreateTaskGroup();
    auto payload = std::make_shared<int>(7);
    const std::weak_ptr<int> weak = payload;
    const NCommon::TaskHandle member = pool.SubmitToGroup(group, [payload](NCommon::TaskContext&) {});
    payload.reset();
    pool.Cancel(member);
    pool.WaitGroup(group);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(pool.GetGroupStatus(group), NCommon::ETaskGroupStatus::CANCELLED);
    resume.release();
    pool.WaitIdle();
}

TEST(TaskGroup, EmptyGroupsOwnershipAndShutdown) {
    NCommon::TaskGroupHandle retained;
    {
        NCommon::TaskSystem pool{1};
        NCommon::TaskSystem other{1};
        const NCommon::TaskGroupHandle group = pool.CreateTaskGroup();
        EXPECT_THROW(other.SealGroup(group), NCommon::Exception);
        EXPECT_THROW(other.CancelGroup(group), NCommon::Exception);
        EXPECT_THROW(other.WaitGroup(group), NCommon::Exception);
        EXPECT_THROW(other.SubmitAfterGroup(group, [](NCommon::TaskContext&) {}), NCommon::Exception);
        EXPECT_THROW(pool.SealGroup({}), NCommon::Exception);
        pool.SealGroup(group);
        pool.SealGroup(group);
        pool.WaitGroup(group);
        pool.CancelGroup(group);
        EXPECT_EQ(pool.GetGroupStatus(group), NCommon::ETaskGroupStatus::COMPLETED);
        retained = pool.CreateTaskGroup();
    }
    EXPECT_TRUE(retained.IsValid());
    NCommon::TaskSystem replacement{1};
    EXPECT_THROW(static_cast<void>(replacement.GetGroupStatus(retained)), NCommon::Exception);
}

TEST(TaskGroup, FailedPrerequisiteAndShutdownRetireEveryMember) {
    std::weak_ptr<int> weak;
    NCommon::TaskGroupHandle retained;
    {
        NCommon::TaskSystem pool{1};
        const NCommon::TaskHandle failed = pool.Submit([](NCommon::TaskContext&) { throw std::runtime_error("x"); });
        pool.Wait(failed);
        retained = pool.CreateTaskGroup();
        auto payload = std::make_shared<int>(7);
        weak = payload;
        const NCommon::TaskHandle dependencies[]{failed};
        const NCommon::TaskHandle member =
                pool.SubmitToGroup(retained, [payload](NCommon::TaskContext&) {}, dependencies);
        payload.reset();
        pool.WaitGroup(retained);
        EXPECT_EQ(pool.GetStatus(member), NCommon::ETaskStatus::CANCELLED);
        EXPECT_EQ(pool.GetGroupStatus(retained), NCommon::ETaskGroupStatus::CANCELLED);
        EXPECT_TRUE(weak.expired());
        // An open group with queued members and waiting continuations is closed by StopWorkers.
        retained = pool.CreateTaskGroup();
        for (std::size_t index = 0; index < 128; ++index) {
            pool.SubmitToGroup(retained, [](NCommon::TaskContext& context) {
                if (!context.IsGroupCancellationRequested()) {
                    context.SpawnInGroup([](NCommon::TaskContext&) {});
                }
            });
        }
        pool.SubmitAfterGroup(retained, [](NCommon::TaskContext&) { FAIL() << "Unsealed group ran continuation"; });
    }
    EXPECT_TRUE(retained.IsValid());
}

TEST(TaskGroup, ConcurrentCancellationAndCompletionPublishOnce) {
    NCommon::TaskSystem pool{8};
    for (std::size_t iteration = 0; iteration < 100; ++iteration) {
        const NCommon::TaskGroupHandle group = pool.CreateTaskGroup();
        for (std::size_t task = 0; task < 16; ++task) {
            pool.SubmitToGroup(group, [](NCommon::TaskContext&) {});
        }
        const NCommon::TaskHandle continuation = pool.SubmitAfterGroup(group, [](NCommon::TaskContext&) {});
        std::thread first{[&] { pool.CancelGroup(group); }};
        std::thread second{[&] { pool.CancelGroup(group); }};
        pool.SealGroup(group);
        first.join();
        second.join();
        pool.WaitGroup(group);
        pool.Wait(continuation);
        pool.WaitIdle();
        const NCommon::ETaskGroupStatus status = pool.GetGroupStatus(group);
        EXPECT_TRUE(status == NCommon::ETaskGroupStatus::COMPLETED || status == NCommon::ETaskGroupStatus::CANCELLED);
    }
}

} // namespace
