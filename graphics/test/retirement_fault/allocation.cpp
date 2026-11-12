#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

#include <graphics/backend/submission_uses.h>
#include <gtest/gtest.h>

namespace {
thread_local std::ptrdiff_t AllocationBudget = -1;
}

// Isolated executable: fail one allocation on the submitting thread only.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library parameter names vary.
void* operator new(std::size_t size) {
    if (AllocationBudget == 0) {
        AllocationBudget = -1;
        throw std::bad_alloc{};
    }
    if (AllocationBudget > 0) {
        --AllocationBudget;
    }
#ifdef GRAPHICS_ENGINE_GRAPHICS_FAULT_ASAN
    // Preserve ASan's operator-new provenance for its default delete interceptor.
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

#ifndef GRAPHICS_ENGINE_GRAPHICS_FAULT_ASAN
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library parameter names vary.
void operator delete(void* memory) noexcept {
    std::free(memory);
}
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library parameter names vary.
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library parameter names vary.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library parameter names vary.
void operator delete(void* memory, const std::nothrow_t&) noexcept {
    ::operator delete(memory);
}
#endif

namespace {
using NGraphics::NBackend::SubmissionRetirementQueue;
using NGraphics::NBackend::SubmissionUseSet;

TEST(SubmissionRetirementFault, EveryPrepareAllocationFailurePreservesEarlierUsesAndCommitNeedsNoAllocation) {
    int failures = 0;
    int successes = 0;
    for (std::ptrdiff_t budget = 0; budget < 12; ++budget) {
        SubmissionRetirementQueue queue;
        auto owner = std::make_shared<const int>(1);
        const std::weak_ptr<const int> lifetime = owner;
        SubmissionUseSet uses{owner};
        queue.Prepare(1, uses).Commit();
        bool failed = false;
        std::ptrdiff_t remaining = -1;
        AllocationBudget = budget;
        try {
            auto prepared = queue.Prepare(2, uses);
            AllocationBudget = 0;
            auto moved = std::move(prepared);
            moved.Commit();
            queue.TakeCompleted(1).clear();
            remaining = AllocationBudget;
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        AllocationBudget = -1;
        owner.reset();
        uses.clear();
        EXPECT_FALSE(lifetime.expired());
        EXPECT_EQ(queue.GetPendingSubmissionCount(), 1U);
        if (failed) {
            ++failures;
            queue.TakeCompleted(1).clear();
            EXPECT_TRUE(lifetime.expired());
        } else {
            ++successes;
            EXPECT_EQ(remaining, 0);
            queue.TakeCompleted(1).clear();
            EXPECT_FALSE(lifetime.expired());
            queue.TakeCompleted(2).clear();
            EXPECT_TRUE(lifetime.expired());
        }
    }
    EXPECT_GT(failures, 0);
    EXPECT_GT(successes, 0);
}
} // namespace
