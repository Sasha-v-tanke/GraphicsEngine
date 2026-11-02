#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <semaphore>
#include <utility>

#include <gtest/gtest.h>
#include <lib/thread/task/task_system.h>

namespace {

thread_local std::ptrdiff_t AllocationBudget = -1;

} // namespace

// This executable is isolated from the other suites. Fail only one allocation on the submitting
// thread, so the capture's reentrant Submit can allocate normally while unwinding.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void* operator new(std::size_t size) {
    if (AllocationBudget == 0) {
        AllocationBudget = -1;
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
        }
        AllocationBudget = -1;
        if (accepted) {
            pool.Cancel(task);
        }
        EXPECT_EQ(destroyed, 1U) << "allocation failure index " << failureIndex;
        finish.release();
        pool.WaitIdle();
        if (accepted) {
            EXPECT_GT(failureIndex, 4);
            return;
        }
    }
    FAIL() << "No successful submission after all injected allocation prefixes";
}

} // namespace
