#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <resources/resource_manager.h>
#include <tests/common/test_error.h>

namespace {

using NResources::EResourceState;
using NResources::ResourceHandle;
using NResources::ResourceManager;

struct Payload final: NCommon::NonTransferable {
    Payload(int value, std::function<void()> onDestroy)
        : Value(value)
        , OnDestroy(std::move(onDestroy)) {
    }
    ~Payload() {
        try {
            OnDestroy();
        } catch (const std::exception& error) {
            ADD_FAILURE() << "Reentrant payload cleanup threw: " << error.what();
        } catch (...) {
            ADD_FAILURE() << "Reentrant payload cleanup threw";
        }
    }
    int Value;
    std::function<void()> OnDestroy;
};

std::shared_ptr<Payload> MakePayload(ResourceManager& resources,
                                     ResourceHandle<Payload> observed,
                                     std::atomic_size_t& destroyed,
                                     int value = 1,
                                     std::optional<EResourceState> expected = std::nullopt) {
    return std::make_shared<Payload>(value, [&resources, observed, &destroyed, expected] {
        ++destroyed;
        const auto state = resources.GetState(observed);
        if (expected) {
            EXPECT_EQ(state, *expected);
        }
        const auto other = resources.Request<int>({"retirement", "reentrant-other"});
        EXPECT_EQ(resources.GetState(other), EResourceState::UNLOADED);
    });
}

TEST(ResourceRetirement, UnloadCommitsBeforeLastOwnerDestructorAndAllowsReentrantRequest) {
    ResourceManager resources;
    std::atomic_size_t destroyed{0};
    const auto handle = resources.Request<Payload>({"payload", "unload"});
    const auto operation = resources.BeginLoading(handle);
    resources.PublishReady(operation, MakePayload(resources, handle, destroyed, 7, EResourceState::UNLOADING));
    EXPECT_EQ(destroyed.load(), 0U);
    resources.RequestUnload(handle);
    EXPECT_EQ(destroyed.load(), 1U);
    EXPECT_FALSE(resources.TryAcquire(handle));
    resources.CompleteUnload(handle);
    resources.Forget(handle);
    EXPECT_EQ(destroyed.load(), 1U);
}

TEST(ResourceRetirement, RejectedPublicationReleasesSolePayloadAfterUnlock) {
    ResourceManager resources;
    ResourceManager foreign;
    std::atomic_size_t destroyed{0};
    const auto observed = resources.Request<Payload>({"payload", "observed"});
    const auto old = resources.BeginLoading(observed);
    resources.Fail(old, {NCommon::make_error_code(NCommon::EError::IO_ERROR), "retry"});
    const auto current = resources.BeginLoading(observed);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] {
        resources.PublishReady(old, MakePayload(resources, observed, destroyed, 1, EResourceState::LOADING));
    });
    EXPECT_EQ(destroyed.load(), 1U);
    EXPECT_TRUE(resources.IsOperationCurrent(current));
    const auto other = foreign.Request<Payload>({"payload", "foreign"});
    const auto foreignOperation = foreign.BeginLoading(other);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        resources.PublishReady(foreignOperation,
                               MakePayload(resources, observed, destroyed, 2, EResourceState::LOADING));
    });
    EXPECT_EQ(destroyed.load(), 2U);
    const auto stale = resources.Request<Payload>({"payload", "forgotten"});
    const auto staleOperation = resources.BeginLoading(stale);
    resources.RequestUnload(stale);
    resources.CompleteUnload(stale);
    resources.Forget(stale);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] {
        resources.PublishReady(staleOperation, MakePayload(resources, observed, destroyed, 3, EResourceState::LOADING));
    });
    EXPECT_EQ(destroyed.load(), 3U);
    // A null stored pointer can still own a custom deleter/control block.
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] {
        resources.PublishReady(current, std::shared_ptr<Payload>{nullptr, [&](Payload*) {
                                                                     ++destroyed;
                                                                     EXPECT_EQ(resources.GetState(observed),
                                                                               EResourceState::LOADING);
                                                                 }});
    });
    EXPECT_EQ(destroyed.load(), 4U);
    resources.PublishReady(current, MakePayload(resources, observed, destroyed, 4, EResourceState::UNLOADING));
    EXPECT_EQ(resources.TryAcquire(observed)->Get()->Value, 4);
    resources.RequestUnload(observed);
    EXPECT_EQ(destroyed.load(), 5U);
}

TEST(ResourceRetirement, AsyncReplacementFailureAndCancellationRetireAfterCommit) {
    for (const int action: {0, 1, 2, 3}) {
        ResourceManager resources;
        std::atomic_size_t destroyed{0};
        const auto handle = resources.Request<Payload>({"payload", "async"});
        const auto operation = resources.BeginLoading(handle);
        const auto expected = action == 0 ? EResourceState::READY
                            : action == 1 ? EResourceState::FAILED
                                          : EResourceState::UNLOADED;
        ASSERT_TRUE(resources.TryAdvance<Payload>(operation,
                                                  EResourceState::LOADING,
                                                  EResourceState::CPU_READY,
                                                  MakePayload(resources, handle, destroyed, 1, expected)));
        if (action == 0) {
            ASSERT_TRUE(resources.TryAdvance<Payload>(operation,
                                                      EResourceState::CPU_READY,
                                                      EResourceState::READY,
                                                      MakePayload(resources, handle, destroyed, 2)));
            EXPECT_EQ(resources.TryAcquire(handle)->Get()->Value, 2);
        } else if (action == 1) {
            ASSERT_TRUE(resources.TryFail(operation, {NCommon::make_error_code(NCommon::EError::IO_ERROR), "fail"}));
        } else if (action == 2) {
            ASSERT_TRUE(resources.TryCancel(operation));
        } else {
            ASSERT_TRUE(resources.TryCancelLoading(handle));
        }
        EXPECT_EQ(destroyed.load(), 1U);
        if (action != 0) {
            const auto retry = resources.BeginLoading(handle);
            resources.PublishReady(retry, MakePayload(resources, handle, destroyed, 3));
        }
        resources.RequestUnload(handle);
        resources.CompleteUnload(handle);
        resources.Forget(handle);
        EXPECT_EQ(destroyed.load(), 2U);
    }
}

TEST(ResourceRetirement, InvalidMutationsKeepThePublishedVersionAndReleaseRejectedInputs) {
    ResourceManager resources;
    std::atomic_size_t destroyed{0};
    std::atomic_size_t rejected{0};
    const auto handle = resources.Request<Payload>({"payload", "invalid-mutations"});
    const auto operation = resources.BeginLoading(handle);
    resources.PublishReady(operation, MakePayload(resources, handle, destroyed, 7, EResourceState::UNLOADING));
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(resources.BeginLoading(handle)); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { resources.CompleteUnload(handle); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { resources.Forget(handle); });
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] {
        resources.Fail(operation, {NCommon::make_error_code(NCommon::EError::IO_ERROR), "invalid"});
    });
    EXPECT_FALSE(resources.TryQueueLoading(handle));
    EXPECT_FALSE(resources.TryCancelLoading(handle));
    EXPECT_FALSE(resources.TryCancel(operation));
    EXPECT_FALSE(resources.TryFail(operation, {NCommon::make_error_code(NCommon::EError::IO_ERROR), "invalid"}));
    EXPECT_FALSE(resources.TryAdvance<Payload>(operation,
                                               EResourceState::READY,
                                               EResourceState::FAILED,
                                               MakePayload(resources, handle, rejected, 9, EResourceState::READY)));
    EXPECT_EQ(rejected.load(), 1U);
    EXPECT_EQ(destroyed.load(), 0U);
    ASSERT_TRUE(resources.TryAcquire(handle));
    EXPECT_EQ(resources.TryAcquire(handle)->Get()->Value, 7);
    resources.RequestUnload(handle);
    resources.CompleteUnload(handle);
    resources.Forget(handle);
    EXPECT_EQ(destroyed.load(), 1U);
}

TEST(ResourceRetirement, LeaseKeepsOldVersionAliveAcrossUnloadAndNewPublication) {
    ResourceManager resources;
    std::atomic_size_t oldDestroyed{0};
    std::atomic_size_t newDestroyed{0};
    const auto handle = resources.Request<Payload>({"payload", "versions"});
    resources.PublishReady(resources.BeginLoading(handle),
                           MakePayload(resources, handle, oldDestroyed, 7, EResourceState::READY));
    auto oldLease = resources.TryAcquire(handle);
    ASSERT_TRUE(oldLease);
    const auto oldVersion = oldLease->GetVersion();
    resources.RequestUnload(handle);
    resources.CompleteUnload(handle);
    EXPECT_EQ(oldDestroyed.load(), 0U);
    EXPECT_EQ(oldLease->Get()->Value, 7);
    EXPECT_FALSE(resources.TryAcquire(handle));
    resources.PublishReady(resources.BeginLoading(handle), MakePayload(resources, handle, newDestroyed, 8));
    EXPECT_GT(resources.TryAcquire(handle)->GetVersion(), oldVersion);
    EXPECT_EQ(resources.TryAcquire(handle)->Get()->Value, 8);
    EXPECT_EQ(oldLease->Get()->Value, 7);
    oldLease.reset();
    EXPECT_EQ(oldDestroyed.load(), 1U);
    resources.RequestUnload(handle);
    resources.CompleteUnload(handle);
    EXPECT_EQ(newDestroyed.load(), 1U);
}

struct AllocationProbe {
    std::function<void()> OnFree;
};

template<typename T>
struct ReentrantAllocator {
    using value_type = T;
    AllocationProbe* Probe;

    explicit ReentrantAllocator(AllocationProbe& probe) noexcept
        : Probe(&probe) {
    }
    template<typename U>
    ReentrantAllocator(const ReentrantAllocator<U>& other) noexcept
        : Probe(other.Probe) {
    }
    T* allocate(std::size_t count) {
        return std::allocator<T>{}.allocate(count);
    }
    template<typename U>
    bool operator==(const ReentrantAllocator<U>& other) const noexcept {
        return Probe == other.Probe;
    }
    void deallocate(T* memory, std::size_t count) noexcept {
        AllocationProbe* const probe = Probe;
        std::allocator<T>{}.deallocate(memory, count);
        try {
            probe->OnFree();
        } catch (const std::exception& error) {
            ADD_FAILURE() << "Reentrant control-block cleanup threw: " << error.what();
        }
    }
};

TEST(ResourceRetirement, LastWeakControlBlockIsAlsoReleasedOutsideMetadataLock) {
    AllocationProbe probe;
    ResourceManager resources;
    std::atomic_size_t freed{0};
    std::atomic_size_t destroyed{0};
    const auto handle = resources.Request<Payload>({"payload", "weak-control-block"});
    auto expected = EResourceState::UNLOADING;
    probe.OnFree = [&] {
        ++freed;
        EXPECT_EQ(resources.GetState(handle), expected);
        static_cast<void>(resources.Request<int>({"retirement", "allocator-reentry"}));
    };
    const auto operation = resources.BeginLoading(handle);
    ASSERT_TRUE(resources.TryAdvance<Payload>(operation,
                                              EResourceState::LOADING,
                                              EResourceState::CPU_READY,
                                              MakePayload(resources, handle, destroyed)));
    auto gpu = std::allocate_shared<int>(ReentrantAllocator<int>{probe}, 5);
    ASSERT_TRUE(resources.TryAdvance<Payload>(operation, EResourceState::CPU_READY, EResourceState::READY, {}, gpu));
    gpu.reset();
    EXPECT_EQ(freed.load(), 0U);
    resources.RequestUnload(handle);
    expected = EResourceState::UNLOADED;
    resources.CompleteUnload(handle);
    EXPECT_EQ(freed.load(), 1U);
    EXPECT_EQ(destroyed.load(), 1U);
}

TEST(ResourceRetirement, ConcurrentAcquisitionObservationAndUnloadPreservePayloadLifetimes) {
    constexpr std::size_t iterations = 100;
    ResourceManager resources;
    std::atomic_size_t destroyed{0};
    std::atomic_bool stop{false};
    const auto handle = resources.Request<Payload>({"payload", "concurrent"});
    std::counting_semaphore<> acquire{0};
    std::counting_semaphore<> acquired{0};
    std::counting_semaphore<> release{0};
    std::counting_semaphore<> released{0};
    std::vector<std::thread> readers;
    readers.reserve(2);
    for (std::size_t reader = 0; reader < 2; ++reader) {
        readers.emplace_back([&] {
            for (std::size_t index = 0; index < iterations; ++index) {
                acquire.acquire();
                auto lease = resources.TryAcquire(handle);
                EXPECT_TRUE(lease.has_value());
                acquired.release();
                release.acquire();
                if (lease) {
                    EXPECT_EQ(lease->Get()->Value, static_cast<int>(index));
                }
                lease.reset();
                released.release();
            }
        });
    }
    std::thread observer([&] {
        while (!stop.load()) {
            static_cast<void>(resources.GetState(handle));
            static_cast<void>(resources.TryAcquire(handle));
        }
    });
    for (std::size_t index = 0; index < iterations; ++index) {
        resources.PublishReady(resources.BeginLoading(handle),
                               MakePayload(resources, handle, destroyed, static_cast<int>(index)));
        acquire.release(2);
        acquired.acquire();
        acquired.acquire();
        resources.RequestUnload(handle);
        resources.CompleteUnload(handle);
        release.release(2);
        released.acquire();
        released.acquire();
    }
    for (auto& reader: readers) {
        reader.join();
    }
    stop.store(true);
    observer.join();
    EXPECT_EQ(destroyed.load(), iterations);
    resources.Forget(handle);
}

} // namespace
