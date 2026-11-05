#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <memory>
#include <new>
#include <semaphore>
#include <utility>

#include <ecs/internal/scoped_batch.h>
#include <ecs/internal/system_run.h>
#include <ecs/world.h>
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
#ifdef GRAPHICS_ENGINE_ECS_FAULT_ASAN
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

#ifndef GRAPHICS_ENGINE_ECS_FAULT_ASAN
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
TEST(EcsRunFault, EveryPrepareAllocationFailureReleasesWorldGuard) {
    NCommon::TaskSystem tasks{1};
    int failures = 0;
    int successes = 0;
    for (std::ptrdiff_t budget = 0; budget < 100; ++budget) {
        auto world = std::make_shared<NEcs::World>();
        world->RegisterSystem({}, [](NEcs::World&) {}, NEcs::SystemSchedule{.Id = 1});
        AllocationBudget = budget;
        try {
            auto prepared = NEcs::NInternal::PrepareSystems(world, tasks, [](const auto&) {});
            AllocationBudget = -1;
            ++successes;
        } catch (const std::bad_alloc&) {
            AllocationBudget = -1;
            ++failures;
        }
        world->RunSystems();
    }
    EXPECT_GT(failures, 0);
    EXPECT_GT(successes, 0);
}

TEST(EcsRunFault, EveryPublicationAllocationFailureDrainsAcceptedPrefixExactlyOnce) {
    int failures = 0;
    int successes = 0;
    for (std::ptrdiff_t budget = 0; budget < 160; ++budget) {
        std::binary_semaphore entered{0};
        std::binary_semaphore release{0};
        NCommon::TaskSystem tasks{1};
        tasks.Submit([&](NCommon::TaskContext&) {
            entered.release();
            release.acquire();
        });
        entered.acquire();
        auto world = std::make_shared<NEcs::World>();
        for (std::uint64_t id = 1; id <= 6; ++id) {
            world->RegisterSystem(
                    {},
                    [](NEcs::World&) {},
                    NEcs::SystemSchedule{.Id = id,
                                         .After = id == 1 ? std::vector<std::uint64_t>{}
                                                          : std::vector<std::uint64_t>{id - 1}});
        }
        std::atomic<int> calls{0};
        std::optional<NEcs::NInternal::SystemRunOutcome> outcome;
        auto prepared = NEcs::NInternal::PrepareSystems(world, tasks, [&](const auto& result) {
            ++calls;
            outcome = result;
        });
        const auto handle = prepared.GetHandle();
        AllocationBudget = budget;
        prepared.Start();
        AllocationBudget = -1;
        release.release();
        tasks.WaitIdle();
        ASSERT_TRUE(handle.IsTerminal());
        EXPECT_EQ(calls.load(), 1);
        ASSERT_TRUE(outcome);
        if (outcome->Status == NEcs::NInternal::ESystemRunStatus::FAILED) {
            EXPECT_TRUE(outcome->Error);
            ++failures;
        } else {
            EXPECT_EQ(outcome->Status, NEcs::NInternal::ESystemRunStatus::COMPLETED);
            ++successes;
        }
        world->RunSystems();
    }
    EXPECT_GT(failures, 0);
    EXPECT_GT(successes, 0);
}

TEST(EcsRunFault, ScopedPrepareAllocationFailuresAreSafeOnWorkers) {
    NCommon::TaskSystem tasks{1};
    int failures = 0;
    int successes = 0;
    for (std::ptrdiff_t budget = 0; budget < 160; ++budget) {
        auto world = std::make_shared<NEcs::World>();
        NEcs::NInternal::ScopedWorldAccess::Register(*world, {}, [](auto&) {}, NEcs::SystemSchedule{.Id = 1});
        (void)tasks.Submit([&, budget](NCommon::TaskContext&) {
            AllocationBudget = budget;
            try {
                auto prepared = NEcs::NInternal::PrepareSystems(world, tasks, [](const auto&) {});
                AllocationBudget = -1;
                ++successes;
            } catch (const std::bad_alloc&) {
                AllocationBudget = -1;
                ++failures;
            }
        });
        tasks.WaitIdle();
        EXPECT_NO_THROW(world->CreateEntity());
    }
    EXPECT_GT(failures, 0);
    EXPECT_GT(successes, 0);
}

TEST(EcsRunFault, ScopedPublicationFailureRetiresReservationsAndLeases) {
    int failures = 0;
    int successes = 0;
    for (std::ptrdiff_t budget = 0; budget < 160; ++budget) {
        std::binary_semaphore entered{0};
        std::binary_semaphore release{0};
        NCommon::TaskSystem tasks{1};
        (void)tasks.Submit([&](NCommon::TaskContext&) {
            entered.release();
            release.acquire();
        });
        entered.acquire();
        auto world = std::make_shared<NEcs::World>();
        const auto entity = world->CreateEntity();
        world->AddComponent<int>(entity, 0);
        for (std::uint64_t id = 1; id <= 6; ++id) {
            NEcs::NInternal::ScopedWorldAccess::Register(
                    *world,
                    {NEcs::NInternal::ScopedWrite<int>(
                            NEcs::NInternal::BatchScope::Entity(world->GetEntityReference(entity)))},
                    [](auto& context) { context.template Write<int>().ForEach([](auto, int& value) { ++value; }); },
                    NEcs::SystemSchedule{.Id = id});
        }
        int calls = 0;
        std::optional<NEcs::NInternal::SystemRunOutcome> outcome;
        const auto weak = std::weak_ptr<NEcs::World>{world};
        auto prepared = NEcs::NInternal::PrepareSystems(world, tasks, [&](const auto& result) {
            EXPECT_TRUE(weak.expired());
            ++calls;
            outcome = result;
        });
        const auto handle = prepared.GetHandle();
        world.reset();
        AllocationBudget = budget;
        prepared.Start();
        AllocationBudget = -1;
        release.release();
        tasks.WaitIdle();
        ASSERT_TRUE(handle.IsTerminal());
        EXPECT_EQ(calls, 1);
        ASSERT_TRUE(outcome);
        if (outcome->Status == NEcs::NInternal::ESystemRunStatus::FAILED) {
            EXPECT_TRUE(outcome->Error);
            ++failures;
        } else {
            EXPECT_EQ(outcome->Status, NEcs::NInternal::ESystemRunStatus::COMPLETED);
            ++successes;
        }
    }
    EXPECT_GT(failures, 0);
    EXPECT_GT(successes, 0);
}
} // namespace
