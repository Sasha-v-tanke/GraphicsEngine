#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <resources/resource_manager.h>
#include <tests/common/test_error.h>

namespace {

thread_local std::ptrdiff_t AllocationBudget = -1;

} // namespace

// Only this executable replaces allocation, and only the requesting thread injects failures.
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size) {
    if (AllocationBudget == 0) {
        AllocationBudget = -1;
        throw std::bad_alloc{};
    }
    if (AllocationBudget > 0) {
        --AllocationBudget;
    }
#ifdef GRAPHICS_ENGINE_RESOURCE_FAULT_ASAN
    // Preserve ASan's operator-new provenance for allocations from the shared C++ runtime.
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

#ifndef GRAPHICS_ENGINE_RESOURCE_FAULT_ASAN
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* memory) noexcept {
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* memory, const std::nothrow_t&) noexcept {
    ::operator delete(memory);
}
#endif

namespace {

using NResources::EResourceState;
using NResources::ResourceHandle;
using NResources::ResourceIdentity;
using NResources::ResourceManager;

void ExerciseLifecycle(ResourceManager& resources, ResourceHandle<int> handle, const ResourceIdentity& identity) {
    EXPECT_EQ(resources.GetIdentity(handle), identity);
    EXPECT_EQ(resources.Request<int>(identity), handle);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT,
                       [&] { static_cast<void>(resources.Request<double>(identity)); });
    const auto operation = resources.BeginLoading(handle);
    resources.PublishReady(operation, std::make_shared<int>(42));
    auto lease = resources.TryAcquire(handle);
    ASSERT_TRUE(lease);
    EXPECT_EQ(**lease, 42);
    resources.RequestUnload(handle);
    EXPECT_FALSE(resources.TryAcquire(handle));
    resources.CompleteUnload(handle);
    resources.Forget(handle);
    EXPECT_EQ(**lease, 42);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(resources.GetState(handle)); });
    const auto next = resources.Request<int>(identity);
    EXPECT_EQ(next.GetSlotIndex(), handle.GetSlotIndex());
    EXPECT_EQ(next.GetGeneration(), handle.GetGeneration() + 2);
    EXPECT_EQ(resources.Request<int>(identity), next);
}

void CheckConcurrentCoalescing(ResourceManager& resources,
                               const ResourceIdentity& identity,
                               ResourceHandle<int> expected) {
    std::array<ResourceHandle<int>, 8> handles;
    std::array<std::jthread, 8> threads;
    for (std::size_t index = 0; index < threads.size(); ++index) {
        threads[index] = std::jthread([&, index] { handles[index] = resources.Request<int>(identity); });
    }
    for (std::jthread& thread: threads) {
        thread.join();
    }
    for (const ResourceHandle<int> handle: handles) {
        EXPECT_EQ(handle, expected);
    }
}

TEST(ResourceRegistrationFault, IndexNodeFailurePreservesReusedSlotAndGeneration) {
    ResourceManager resources;
    const auto stable = resources.Request<int>({"test", "stable"});
    const auto stableOperation = resources.BeginLoading(stable);
    resources.PublishReady(stableOperation, std::make_shared<int>(7));
    const auto old = resources.Request<int>({"test", "old"});
    resources.Forget(old);
    ResourceIdentity identity{"test", "new"};
    // Short strings need no allocation and the vector and hash buckets already have capacity.
    // The first allocation can only be the new identity-index node.
    AllocationBudget = 0;
    bool failed = false;
    try {
        static_cast<void>(resources.Request<int>(std::move(identity)));
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    AllocationBudget = -1;
    ASSERT_TRUE(failed);
    EXPECT_EQ(resources.GetState(stable), EResourceState::READY);
    EXPECT_EQ(*resources.GetCpuResource(stable), 7);
    const auto recovered = resources.Request<int>({"test", "new"});
    EXPECT_EQ(recovered.GetSlotIndex(), old.GetSlotIndex());
    EXPECT_EQ(recovered.GetGeneration(), old.GetGeneration() + 2);
    CheckConcurrentCoalescing(resources, {"test", "new"}, recovered);
    ExerciseLifecycle(resources, recovered, {"test", "new"});
}

TEST(ResourceRegistrationFault, EveryAllocationPrefixPreservesNewAndReusedSlots) {
    for (const bool reuse: {false, true}) {
        bool reachedSuccess = false;
        std::size_t failures = 0;
        for (std::ptrdiff_t failureIndex = 0; failureIndex < 32; ++failureIndex) {
            ResourceManager resources;
            const auto stable = resources.Request<int>({"test", "stable"});
            const auto operation = resources.BeginLoading(stable);
            resources.PublishReady(operation, std::make_shared<int>(7));
            std::uint64_t expectedGeneration = 1;
            if (reuse) {
                const auto old = resources.Request<int>({"test", "old"});
                resources.Forget(old);
                expectedGeneration = old.GetGeneration() + 2;
            }
            const ResourceIdentity identity{std::string(128, 'c'), std::string(256, 'k')};
            ResourceHandle<int> requested;
            bool failed = false;
            AllocationBudget = failureIndex;
            try {
                requested = resources.Request<int>(identity);
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            AllocationBudget = -1;
            SCOPED_TRACE(::testing::Message() << "reuse=" << reuse << " failureIndex=" << failureIndex);
            EXPECT_EQ(resources.GetState(stable), EResourceState::READY);
            EXPECT_EQ(*resources.GetCpuResource(stable), 7);
            EXPECT_EQ(resources.Request<int>({"test", "stable"}), stable);
            const auto recovered = resources.Request<int>(identity);
            EXPECT_EQ(recovered.GetSlotIndex(), 1U);
            EXPECT_EQ(recovered.GetGeneration(), expectedGeneration);
            if (!failed) {
                EXPECT_EQ(requested, recovered);
            }
            ExerciseLifecycle(resources, recovered, identity);
            if (!failed) {
                reachedSuccess = true;
                break;
            }
            ++failures;
        }
        EXPECT_TRUE(reachedSuccess);
        EXPECT_GT(failures, 0U);
    }
}

TEST(ResourceRegistrationFault, ConcurrentFirstRequestsCoalesceAfterFailedInsertion) {
    for (std::size_t cycle = 0; cycle < 50; ++cycle) {
        ResourceManager resources;
        const auto old = resources.Request<int>({"test", "old"});
        resources.Forget(old);
        AllocationBudget = 0;
        bool failed = false;
        try {
            static_cast<void>(resources.Request<int>({"test", "new"}));
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        AllocationBudget = -1;
        ASSERT_TRUE(failed);
        std::array<ResourceHandle<int>, 8> handles;
        std::array<std::jthread, 8> threads;
        for (std::size_t index = 0; index < threads.size(); ++index) {
            threads[index] = std::jthread([&, index] { handles[index] = resources.Request<int>({"test", "new"}); });
        }
        for (std::jthread& thread: threads) {
            thread.join();
        }
        for (const ResourceHandle<int> handle: handles) {
            EXPECT_EQ(handle, handles.front());
            EXPECT_EQ(handle.GetSlotIndex(), old.GetSlotIndex());
            EXPECT_EQ(handle.GetGeneration(), old.GetGeneration() + 2);
        }
        ExerciseLifecycle(resources, handles.front(), {"test", "new"});
    }
}

} // namespace
