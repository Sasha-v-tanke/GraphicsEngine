#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
#include <semaphore>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <lib/thread/task/task_system.h>

namespace {

thread_local std::ptrdiff_t AllocationBudget = -1;
thread_local bool FailEveryAllocation = false;

} // namespace

// This executable is isolated from the other suites. Fail only one allocation on the submitting
// thread, so the capture's reentrant Submit can allocate normally while unwinding.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void* operator new(std::size_t size) {
    if (AllocationBudget == 0) {
        if (!FailEveryAllocation) {
            AllocationBudget = -1;
        }
        throw std::bad_alloc{};
    }
    if (AllocationBudget > 0) {
        --AllocationBudget;
    }
#ifdef GRAPHICS_ENGINE_TASK_FAULT_ASAN
    // ASan's nothrow interceptor allocates with operator-new provenance. Its default delete
    // must release these blocks, including allocations made inside the shared C++ runtime.
    if (void* memory = ::operator new(size, std::nothrow)) {
        return memory;
    }
#else
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
#endif
    throw std::bad_alloc{};
}

#ifndef GRAPHICS_ENGINE_TASK_FAULT_ASAN
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void operator delete(void* memory) noexcept {
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void operator delete(void* memory, const std::nothrow_t&) noexcept {
    ::operator delete(memory);
}
#endif

namespace NCommon {

// Test-only access to the private Q-pop/S-claim boundary, without a production callback hook.
struct TaskSystemTestAccess {
    static auto Pop(TaskSystem& pool) {
        return pool.m_readyScheduler.TryPopReadyTask(WorkerIndex{0});
    }
    static bool Claim(const auto& state) {
        TaskHandle handle;
        TaskSystem::TaskFunction function;
        return TaskSystem::TaskLifetime::TryClaimReadyTask(state, handle, function);
    }
    static bool IsDrained(TaskSystem& pool) {
        std::lock_guard lock{pool.m_mutex};
        return pool.m_registry.ActiveTasks.empty() && pool.m_lifetime.OutstandingTasks.load() == 0 &&
               pool.m_readyScheduler.ReadyTaskCount.load() == 0;
    }
};

} // namespace NCommon

namespace {

TEST(TaskRetirementFault, NothrowAllocationUsesMatchingRelease) {
    std::unique_ptr<int> value{new (std::nothrow) int{7}};
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 7);
}

struct RollbackProbe {
    NCommon::TaskSystem* Pool = nullptr;
    std::size_t* Destroyed = nullptr;

    ~RollbackProbe() {
        ++*Destroyed;
        Pool->Submit([](NCommon::TaskContext&) {});
    }
};

TEST(TaskRetirementFault, ReentrantCaptureSurvivesEverySubmitAllocationFailure) {
    bool exercisedRollback = false;
    for (std::ptrdiff_t failureIndex = 0; failureIndex < 128; ++failureIndex) {
        // The semaphores outlive the pool, including on any test failure.
        std::binary_semaphore entered{0};
        std::binary_semaphore finish{0};
        NCommon::TaskSystem pool{1};
        pool.Submit([&](NCommon::TaskContext&) {
            entered.release();
            finish.acquire();
        });
        entered.acquire();
        const NCommon::TaskHandle parents[]{pool.Submit([](NCommon::TaskContext&) {}),
                                            pool.Submit([](NCommon::TaskContext&) {})};
        std::size_t destroyed = 0;
        auto probe = std::make_shared<RollbackProbe>();
        probe->Pool = &pool;
        probe->Destroyed = &destroyed;
        bool accepted = false;
        NCommon::TaskHandle task;
        AllocationBudget = failureIndex;
        try {
            task = pool.Submit([probe = std::move(probe)](NCommon::TaskContext&) {}, parents);
            accepted = true;
        } catch (const std::bad_alloc&) {
            exercisedRollback = true;
        }
        AllocationBudget = -1;
        if (accepted) {
            pool.Cancel(task);
        }
        EXPECT_EQ(destroyed, 1U) << "allocation failure index " << failureIndex;
        EXPECT_EQ(pool.GetStatus(parents[0]), NCommon::ETaskStatus::READY);
        EXPECT_EQ(pool.GetStatus(parents[1]), NCommon::ETaskStatus::READY);
        finish.release();
        pool.WaitIdle();
        if (accepted) {
            // Stable edges remove allocations; the contract is rollback at every failing prefix.
            EXPECT_TRUE(exercisedRollback);
            return;
        }
    }
    FAIL() << "No successful submission after all injected allocation prefixes";
}

TEST(TaskGroupFault, AdmissionRollbackHasNoLeakedContribution) {
    for (std::ptrdiff_t failureIndex = 0; failureIndex < 128; ++failureIndex) {
        NCommon::TaskSystem pool{1};
        const NCommon::TaskGroupHandle group = pool.CreateTaskGroup();
        bool accepted = false;
        NCommon::TaskHandle member;
        AllocationBudget = failureIndex;
        try {
            member = pool.SubmitToGroup(group, [](NCommon::TaskContext&) {});
            accepted = true;
        } catch (const std::bad_alloc&) {
        }
        AllocationBudget = -1;
        pool.SealGroup(group);
        pool.WaitGroup(group);
        pool.WaitIdle();
        EXPECT_EQ(pool.GetGroupStatus(group), NCommon::ETaskGroupStatus::COMPLETED);
        if (accepted) {
            EXPECT_EQ(pool.GetStatus(member), NCommon::ETaskStatus::COMPLETED);
            return;
        }
    }
    FAIL() << "No successful group submission";
}

TEST(TaskGroupFault, CreationAndContinuationRollback) {
    for (std::ptrdiff_t failureIndex = 0; failureIndex < 128; ++failureIndex) {
        NCommon::TaskSystem pool{1};
        NCommon::TaskGroupHandle group;
        AllocationBudget = failureIndex;
        try {
            group = pool.CreateTaskGroup();
        } catch (const std::bad_alloc&) {
        }
        AllocationBudget = -1;
        if (group.IsValid()) {
            pool.CancelGroup(group);
        }
        pool.WaitIdle();
        if (group.IsValid()) {
            break;
        }
    }
    for (std::ptrdiff_t failureIndex = 0; failureIndex < 128; ++failureIndex) {
        NCommon::TaskSystem pool{1};
        const NCommon::TaskGroupHandle group = pool.CreateTaskGroup();
        NCommon::TaskHandle continuation;
        AllocationBudget = failureIndex;
        try {
            continuation = pool.SubmitAfterGroup(group, [](NCommon::TaskContext&) {});
        } catch (const std::bad_alloc&) {
        }
        AllocationBudget = -1;
        pool.SealGroup(group);
        pool.WaitIdle();
        EXPECT_EQ(pool.GetGroupStatus(group), NCommon::ETaskGroupStatus::COMPLETED);
        if (continuation.IsValid()) {
            EXPECT_EQ(pool.GetStatus(continuation), NCommon::ETaskStatus::COMPLETED);
            return;
        }
    }
    FAIL() << "No successful continuation submission";
}

TEST(TaskGroupFault, FanInAndCancellationNeedNoAllocation) {
    NCommon::TaskSystem pool{1};
    const NCommon::TaskGroupHandle group = pool.CreateTaskGroup();
    const NCommon::TaskHandle continuation = pool.SubmitAfterGroup(group, [](NCommon::TaskContext&) {});
    AllocationBudget = 0;
    pool.SealGroup(group);
    AllocationBudget = -1;
    pool.Wait(continuation);
    EXPECT_EQ(pool.GetStatus(continuation), NCommon::ETaskStatus::COMPLETED);

    const NCommon::TaskGroupHandle cancelled = pool.CreateTaskGroup();
    const NCommon::TaskHandle cancelledContinuation = pool.SubmitAfterGroup(cancelled, [](NCommon::TaskContext&) {});
    AllocationBudget = 0;
    pool.CancelGroup(cancelled);
    AllocationBudget = -1;
    pool.WaitGroup(cancelled);
    pool.WaitIdle();
    EXPECT_EQ(pool.GetStatus(cancelledContinuation), NCommon::ETaskStatus::CANCELLED);
}

TEST(TaskDagFault, CancellationDetachesSharedEdgesWithoutAllocating) {
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    NCommon::TaskSystem pool{1};
    pool.Submit([&](NCommon::TaskContext&) {
        entered.release();
        resume.acquire();
    });
    entered.acquire();
    const NCommon::TaskHandle root = pool.Submit([](NCommon::TaskContext&) {});
    const NCommon::TaskHandle unaffected = pool.Submit([](NCommon::TaskContext&) {});
    const NCommon::TaskHandle parents[]{root, unaffected};
    const NCommon::TaskHandle left = pool.Submit([](NCommon::TaskContext&) {}, parents);
    const NCommon::TaskHandle right = pool.Submit([](NCommon::TaskContext&) {}, parents);
    const NCommon::TaskHandle joinedParents[]{root, left, right};
    const NCommon::TaskHandle join = pool.Submit([](NCommon::TaskContext&) {}, joinedParents);
    bool allocated = false;
    AllocationBudget = 0;
    try {
        pool.Cancel(root);
        allocated = AllocationBudget != 0;
    } catch (const std::bad_alloc&) {
        allocated = true;
    }
    AllocationBudget = -1;
    EXPECT_FALSE(allocated);
    EXPECT_EQ(pool.GetStatus(join), NCommon::ETaskStatus::CANCELLED);
    EXPECT_EQ(pool.GetStatus(unaffected), NCommon::ETaskStatus::READY);
    resume.release();
    pool.WaitIdle();
}

TEST(TaskTerminalFault, SuccessFanOutAndReadyPublicationCannotAllocate) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        std::binary_semaphore entered{0};
        std::binary_semaphore resume{0};
        NCommon::TaskSystem pool{workers};
        const auto root = pool.Submit([&](NCommon::TaskContext&) {
            entered.release();
            resume.acquire();
            FailEveryAllocation = true;
            AllocationBudget = 0;
        });
        entered.acquire();
        std::atomic_size_t invoked = 0;
        std::vector<NCommon::TaskHandle> children;
        children.reserve(128);
        for (std::size_t index = 0; index < 128; ++index) {
            children.push_back(pool.Submit(
                    [&](NCommon::TaskContext&) {
                        FailEveryAllocation = true;
                        AllocationBudget = 0;
                        invoked.fetch_add(1);
                    },
                    std::span{&root, 1}));
        }
        const auto joined = pool.Submit([](NCommon::TaskContext&) {}, children);
        resume.release();
        pool.Wait(joined);
        pool.WaitIdle();
        EXPECT_EQ(invoked.load(), 128U);
        EXPECT_EQ(pool.GetStatus(root), NCommon::ETaskStatus::COMPLETED);
        EXPECT_TRUE(NCommon::TaskSystemTestAccess::IsDrained(pool));
        // Reuse workers that may still reject every allocation on their own thread.
        const auto reused = pool.Submit([](NCommon::TaskContext&) {});
        pool.Wait(reused);
    }
}

TEST(TaskTerminalFault, ErrorInfoAllocationFailureStillPublishesOneFailure) {
#if defined(GRAPHICS_ENGINE_TASK_FAULT_ASAN) && defined(__APPLE__)
    // macOS ASan interposes libc++'s dylib allocations before this executable's operator new.
    // Debug and unsanitized Release exercise this fail index; do not report a false ASan pass.
    GTEST_SKIP() << "macOS ASan bypasses the test allocator for libc++ string allocation";
#endif
    const auto failure = std::make_exception_ptr(std::runtime_error(std::string(1024, 'x')));
    for (const std::size_t workers: {1U, 2U, 8U}) {
        std::binary_semaphore entered{0};
        std::binary_semaphore resume{0};
        NCommon::TaskSystem pool{workers};
        const auto root = pool.Submit([&](NCommon::TaskContext&) {
            entered.release();
            resume.acquire();
            FailEveryAllocation = true;
            AllocationBudget = 0;
            std::rethrow_exception(failure);
        });
        entered.acquire();
        const auto dependent = pool.Submit([](NCommon::TaskContext&) {}, std::span{&root, 1});
        resume.release();
        pool.WaitIdle();
        EXPECT_EQ(pool.GetStatus(root), NCommon::ETaskStatus::FAILED);
        ASSERT_TRUE(pool.GetError(root).has_value());
        EXPECT_EQ(pool.GetError(root)->Code, NCommon::make_error_code(NCommon::EError::OUT_OF_MEMORY));
        EXPECT_TRUE(pool.GetError(root)->Message.empty());
        EXPECT_EQ(pool.GetStatus(dependent), NCommon::ETaskStatus::CANCELLED);
        EXPECT_TRUE(NCommon::TaskSystemTestAccess::IsDrained(pool));
    }
}

TEST(TaskTerminalFault, PoppedLeaseSurvivesCancelRetirementAndPoolDestruction) {
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    auto pool = std::make_unique<NCommon::TaskSystem>(1);
    pool->Submit([&](NCommon::TaskContext&) {
        entered.release();
        resume.acquire();
    });
    entered.acquire();
    auto task = pool->Submit([](NCommon::TaskContext&) {});
    auto popped = NCommon::TaskSystemTestAccess::Pop(*pool);
    ASSERT_NE(popped, nullptr);
    pool->Cancel(task);
    task = {};
    resume.release();
    pool->WaitIdle();
    EXPECT_TRUE(NCommon::TaskSystemTestAccess::IsDrained(*pool));
    pool.reset();
    EXPECT_FALSE(NCommon::TaskSystemTestAccess::Claim(popped));
}

TEST(TaskTerminalFault, ThousandsOfCancelledReadyTasksLeaveNoMembershipOrWakeCredits) {
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    NCommon::TaskSystem pool{1};
    pool.Submit([&](NCommon::TaskContext&) {
        entered.release();
        resume.acquire();
    });
    entered.acquire();
    for (std::size_t index = 0; index < 4096; ++index) {
        const auto task = pool.Submit([](NCommon::TaskContext&) {});
        AllocationBudget = 0;
        FailEveryAllocation = true;
        pool.Cancel(task);
        const bool allocated = AllocationBudget != 0;
        FailEveryAllocation = false;
        AllocationBudget = -1;
        EXPECT_FALSE(allocated);
    }
    const auto fresh = pool.Submit([](NCommon::TaskContext&) {});
    resume.release();
    pool.Wait(fresh);
    pool.WaitIdle();
    EXPECT_TRUE(NCommon::TaskSystemTestAccess::IsDrained(pool));
}

TEST(TaskTerminalFault, EmptyRegistrySubmitFailsCleanlyAtEveryPrepareIndex) {
    bool failed = false;
    for (std::ptrdiff_t failureIndex = 0; failureIndex < 128; ++failureIndex) {
        NCommon::TaskSystem pool{1};
        NCommon::TaskHandle task;
        AllocationBudget = failureIndex;
        try {
            task = pool.Submit([](NCommon::TaskContext&) {});
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        AllocationBudget = -1;
        pool.WaitIdle();
        EXPECT_TRUE(NCommon::TaskSystemTestAccess::IsDrained(pool));
        const auto reused = pool.Submit([](NCommon::TaskContext&) {});
        pool.Wait(reused);
        if (task.IsValid()) {
            EXPECT_TRUE(failed);
            EXPECT_EQ(pool.GetStatus(task), NCommon::ETaskStatus::COMPLETED);
            return;
        }
    }
    FAIL() << "No successful Submit after all allocation prefixes";
}

struct StopRelease {
    std::counting_semaphore<>* Resume = nullptr;
    ~StopRelease() {
        Resume->release();
    }
};

TEST(TaskTerminalFault, StopDrainsPendingPayloadsWithoutAllocating) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        std::counting_semaphore<> entered{0};
        std::counting_semaphore<> resume{0};
        auto pool = std::make_unique<NCommon::TaskSystem>(workers);
        for (std::size_t index = 0; index < workers; ++index) {
            pool->Submit([&](NCommon::TaskContext&) {
                entered.release();
                resume.acquire();
            });
        }
        for (std::size_t index = 0; index < workers; ++index) {
            entered.acquire();
            auto release = std::make_shared<StopRelease>();
            release->Resume = &resume;
            pool->Submit([release = std::move(release)](NCommon::TaskContext&) {});
        }
        AllocationBudget = 0;
        FailEveryAllocation = true;
        pool.reset();
        const bool allocated = AllocationBudget != 0;
        FailEveryAllocation = false;
        AllocationBudget = -1;
        EXPECT_FALSE(allocated);
    }
}

} // namespace
