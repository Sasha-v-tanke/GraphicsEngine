#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <lib/thread/task/task_system.h>

namespace {

struct Payload {
    explicit Payload(std::atomic_size_t& destroyed)
        : Destroyed(destroyed) {
    }
    ~Payload() {
        ++Destroyed;
    }
    std::atomic_size_t& Destroyed;
};

NCommon::TaskSystem::TaskFunction Counted(std::atomic_size_t& destroyed, std::atomic_size_t& invoked) {
    return [payload = std::make_shared<Payload>(destroyed), &invoked](NCommon::TaskContext&) { ++invoked; };
}

class PausedWorkers final {
public:
    explicit PausedWorkers(NCommon::TaskSystem& pool)
        : m_pool(pool) {
        for (std::size_t worker = 0; worker < pool.GetWorkerCount(); ++worker) {
            pool.Submit([this](NCommon::TaskContext&) {
                m_entered.release();
                m_resume.acquire();
            });
        }
        for (std::size_t worker = 0; worker < pool.GetWorkerCount(); ++worker) {
            m_entered.acquire();
        }
    }
    ~PausedWorkers() {
        m_resume.release(static_cast<std::ptrdiff_t>(m_pool.GetWorkerCount()));
        m_pool.WaitIdle();
    }

private:
    NCommon::TaskSystem& m_pool;
    std::counting_semaphore<> m_entered{0};
    std::counting_semaphore<> m_resume{0};
};

TEST(TaskDag, TransitiveAndNestedDiamonds) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        std::atomic_size_t destroyed{0};
        std::atomic_size_t invoked{0};
        NCommon::TaskSystem pool{workers};
        PausedWorkers paused{pool};
        std::vector<NCommon::TaskHandle> tasks;
        tasks.push_back(pool.Submit(Counted(destroyed, invoked)));
        for (std::size_t level = 0; level < 24; ++level) {
            const NCommon::TaskHandle parent[]{tasks.back()};
            const NCommon::TaskHandle left = pool.Submit(Counted(destroyed, invoked), parent);
            const NCommon::TaskHandle right = pool.Submit(Counted(destroyed, invoked), parent);
            // Includes A->B, A->C, B->C and a shared descendant at every level.
            const NCommon::TaskHandle parents[]{parent[0], left, right};
            tasks.push_back(left);
            tasks.push_back(right);
            tasks.push_back(pool.Submit(Counted(destroyed, invoked), parents));
        }
        pool.Cancel(tasks.front());
        pool.Cancel(tasks.front());
        for (const NCommon::TaskHandle& task: tasks) {
            pool.Wait(task);
            EXPECT_EQ(pool.GetStatus(task), NCommon::ETaskStatus::CANCELLED);
        }
        EXPECT_EQ(invoked.load(), 0U);
        EXPECT_EQ(destroyed.load(), tasks.size());
    }
}

TEST(TaskDag, FailureCancelsSharedDescendants) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        std::atomic_size_t destroyed{0};
        std::atomic_size_t invoked{0};
        std::binary_semaphore entered{0};
        std::binary_semaphore resume{0};
        NCommon::TaskSystem pool{workers};
        const NCommon::TaskHandle root = pool.Submit([&](NCommon::TaskContext&) {
            entered.release();
            resume.acquire();
            throw std::runtime_error("DAG root failed");
        });
        entered.acquire();
        std::vector<NCommon::TaskHandle> tasks;
        for (std::size_t branch = 0; branch < 16; ++branch) {
            const NCommon::TaskHandle parents[]{root};
            tasks.push_back(pool.Submit(Counted(destroyed, invoked), parents));
        }
        const NCommon::TaskHandle join = pool.Submit(Counted(destroyed, invoked), tasks);
        const NCommon::TaskHandle finalParents[]{root, tasks.front(), join};
        tasks.push_back(join);
        tasks.push_back(pool.Submit(Counted(destroyed, invoked), finalParents));
        resume.release();
        pool.WaitIdle();
        EXPECT_EQ(pool.GetStatus(root), NCommon::ETaskStatus::FAILED);
        ASSERT_TRUE(pool.GetError(root).has_value());
        EXPECT_EQ(pool.GetError(root)->Message, "DAG root failed");
        for (const NCommon::TaskHandle& task: tasks) {
            EXPECT_EQ(pool.GetStatus(task), NCommon::ETaskStatus::CANCELLED);
        }
        EXPECT_EQ(destroyed.load(), tasks.size());
        EXPECT_EQ(invoked.load(), 0U);
    }
}

TEST(TaskDag, SharedFanInPreservesUnaffectedBacklinks) {
    std::atomic_size_t destroyed{0};
    std::atomic_size_t invoked{0};
    NCommon::TaskSystem pool{2};
    const NCommon::TaskHandle independent = [&] {
        PausedWorkers paused{pool};
        const NCommon::TaskHandle cancelled = pool.Submit([](NCommon::TaskContext&) {});
        const NCommon::TaskHandle unaffected = pool.Submit([](NCommon::TaskContext&) {});
        const NCommon::TaskHandle parents[]{cancelled, unaffected};
        std::vector<NCommon::TaskHandle> joins;
        joins.reserve(128);
        for (std::size_t branch = 0; branch < 128; ++branch) {
            joins.push_back(pool.Submit(Counted(destroyed, invoked), parents));
        }
        const NCommon::TaskHandle survivingParents[]{unaffected};
        const NCommon::TaskHandle survivor = pool.Submit([](NCommon::TaskContext&) {}, survivingParents);
        pool.Cancel(cancelled);
        for (const NCommon::TaskHandle& join: joins) {
            EXPECT_EQ(pool.GetStatus(join), NCommon::ETaskStatus::CANCELLED);
        }
        EXPECT_EQ(pool.GetStatus(unaffected), NCommon::ETaskStatus::READY);
        EXPECT_EQ(pool.GetStatus(survivor), NCommon::ETaskStatus::WAITING);
        EXPECT_EQ(destroyed.load(), joins.size());
        return survivor;
    }();
    pool.Wait(independent);
    EXPECT_EQ(pool.GetStatus(independent), NCommon::ETaskStatus::COMPLETED);
    EXPECT_EQ(invoked.load(), 0U);
}

TEST(TaskDag, ConcurrentCompletionAndCancellation) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        NCommon::TaskSystem pool{workers};
        for (std::size_t iteration = 0; iteration < 100; ++iteration) {
            std::atomic_size_t destroyed{0};
            std::atomic_size_t invoked{0};
            std::binary_semaphore entered{0};
            std::binary_semaphore resume{0};
            const NCommon::TaskHandle root = pool.Submit([&](NCommon::TaskContext&) {
                entered.release();
                resume.acquire();
            });
            entered.acquire();
            const NCommon::TaskHandle parents[]{root};
            const NCommon::TaskHandle left = pool.Submit(Counted(destroyed, invoked), parents);
            const NCommon::TaskHandle right = pool.Submit(Counted(destroyed, invoked), parents);
            const NCommon::TaskHandle joinParents[]{root, left, right};
            const NCommon::TaskHandle join = pool.Submit(Counted(destroyed, invoked), joinParents);
            std::thread canceller{[&] {
                try {
                    pool.Cancel(left);
                } catch (const NCommon::Exception& error) {
                    EXPECT_EQ(error.code(), NCommon::make_error_code(NCommon::EError::INVALID_STATE));
                }
            }};
            resume.release();
            canceller.join();
            pool.Wait(join);
            pool.WaitIdle();
            EXPECT_EQ(destroyed.load(), 3U);
            EXPECT_LE(invoked.load(), 3U);
            if (pool.GetStatus(left) == NCommon::ETaskStatus::CANCELLED) {
                EXPECT_EQ(pool.GetStatus(join), NCommon::ETaskStatus::CANCELLED);
            }
        }
    }
}

TEST(TaskDag, ShutdownDuringFailureCascade) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        std::binary_semaphore entered{0};
        std::binary_semaphore resumeRoot{0};
        std::binary_semaphore cleanupEntered{0};
        std::binary_semaphore resumeCleanup{0};
        std::binary_semaphore stopStarted{0};
        std::atomic_size_t destroyed{0};
        std::atomic_size_t invoked{0};
        auto pool = std::make_unique<NCommon::TaskSystem>(workers);
        const NCommon::TaskHandle root = pool->Submit([&](NCommon::TaskContext&) {
            entered.release();
            resumeRoot.acquire();
            throw std::runtime_error("shutdown cascade");
        });
        entered.acquire();
        struct PausedCleanup {
            PausedCleanup(std::binary_semaphore& entered, std::binary_semaphore& resume)
                : Entered(entered)
                , Resume(resume) {
            }
            ~PausedCleanup() {
                Entered.release();
                Resume.acquire();
            }
            std::binary_semaphore& Entered;
            std::binary_semaphore& Resume;
        };
        const NCommon::TaskHandle parents[]{root};
        pool->Submit(
                [payload = std::make_shared<PausedCleanup>(cleanupEntered, resumeCleanup)](NCommon::TaskContext&) {},
                parents);
        std::vector<NCommon::TaskHandle> tasks;
        tasks.reserve(64);
        for (std::size_t index = 0; index < 64; ++index) {
            tasks.push_back(pool->Submit(Counted(destroyed, invoked), parents));
        }
        pool->Submit([](NCommon::TaskContext&) {}, tasks);
        resumeRoot.release();
        cleanupEntered.acquire();
        std::thread stopper{[owned = std::move(pool), &stopStarted]() mutable {
            stopStarted.release();
            owned.reset();
        }};
        stopStarted.acquire();
        resumeCleanup.release();
        stopper.join();
        EXPECT_EQ(destroyed.load(), tasks.size());
        EXPECT_EQ(invoked.load(), 0U);
    }
}

TEST(TaskDag, DeepChain200k) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        std::atomic_size_t destroyed{0};
        std::atomic_size_t invoked{0};
        NCommon::TaskSystem pool{workers};
        PausedWorkers paused{pool};
        std::vector<NCommon::TaskHandle> tasks;
        tasks.reserve(200000);
        tasks.push_back(pool.Submit(Counted(destroyed, invoked)));
        for (std::size_t index = 1; index < 200000; ++index) {
            const NCommon::TaskHandle parents[]{tasks.back()};
            tasks.push_back(pool.Submit(Counted(destroyed, invoked), parents));
        }
        const auto start = std::chrono::steady_clock::now();
        pool.Cancel(tasks.front());
        const auto elapsed = std::chrono::steady_clock::now() - start;
        RecordProperty("cancel_us_workers_" + std::to_string(workers),
                       std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
        for (const NCommon::TaskHandle& task: tasks) {
            EXPECT_EQ(pool.GetStatus(task), NCommon::ETaskStatus::CANCELLED);
        }
        EXPECT_EQ(destroyed.load(), tasks.size());
        EXPECT_EQ(invoked.load(), 0U);
    }
}

TEST(TaskDag, WideFanIn100k) {
    std::atomic_size_t destroyed{0};
    std::atomic_size_t invoked{0};
    NCommon::TaskSystem pool{8};
    PausedWorkers paused{pool};
    const NCommon::TaskHandle root = pool.Submit([](NCommon::TaskContext&) {});
    const NCommon::TaskHandle unaffected = pool.Submit([](NCommon::TaskContext&) {});
    const NCommon::TaskHandle parents[]{root, unaffected};
    std::vector<NCommon::TaskHandle> joins;
    joins.reserve(100000);
    for (std::size_t index = 0; index < 100000; ++index) {
        joins.push_back(pool.Submit(Counted(destroyed, invoked), parents));
    }
    pool.Cancel(root);
    for (const NCommon::TaskHandle& join: joins) {
        EXPECT_EQ(pool.GetStatus(join), NCommon::ETaskStatus::CANCELLED);
    }
    EXPECT_EQ(pool.GetStatus(unaffected), NCommon::ETaskStatus::READY);
    EXPECT_EQ(destroyed.load(), joins.size());
    EXPECT_EQ(invoked.load(), 0U);
}

} // namespace
