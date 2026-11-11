#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <new>
#include <semaphore>
#include <utility>

#include <ecs/internal/adaptive_traversal.h>
#include <ecs/internal/scoped_batch.h>
#include <ecs/internal/system_run.h>
#include <ecs/internal/world_changes.h>
#include <ecs/world.h>
#include <gtest/gtest.h>
#include <lib/thread/task/task_system.h>

namespace {

thread_local std::ptrdiff_t AllocationBudget = -1;

#ifndef GRAPHICS_ENGINE_ECS_FAULT_ASAN
// Allocation-request bytes, including containers/control blocks, rather than logical entry counts.
// The probe itself uses fixed stack storage and cannot recursively allocate from operator new.
struct HeapProbe {
    struct Record {
        void* Pointer = nullptr;
        std::size_t Bytes = 0;
    };
    std::array<Record, 16384> Records = {};
    std::size_t LiveBytes = 0;
    std::size_t PeakBytes = 0;
    bool Overflow = false;

    void Allocate(void* pointer, std::size_t bytes) noexcept {
        for (auto& record: Records) {
            if (record.Pointer == nullptr) {
                record = {pointer, bytes};
                LiveBytes += bytes;
                PeakBytes = std::max(PeakBytes, LiveBytes);
                return;
            }
        }
        Overflow = true;
    }

    void Free(void* pointer) noexcept {
        for (auto& record: Records) {
            if (record.Pointer == pointer && pointer != nullptr) {
                LiveBytes -= record.Bytes;
                record = {};
                return;
            }
        }
    }
};

thread_local HeapProbe* ActiveHeapProbe = nullptr;

class HeapProbeScope final: private NCommon::NonTransferable {
public:
    explicit HeapProbeScope(HeapProbe& probe)
        : m_previous(std::exchange(ActiveHeapProbe, &probe)) {
    }
    ~HeapProbeScope() {
        ActiveHeapProbe = m_previous;
    }

private:
    HeapProbe* m_previous;
};
#endif

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
        if (ActiveHeapProbe != nullptr) {
            ActiveHeapProbe->Allocate(memory, size == 0 ? 1 : size);
        }
        return memory;
    }
#endif
    throw std::bad_alloc{};
}

#ifndef GRAPHICS_ENGINE_ECS_FAULT_ASAN
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void operator delete(void* memory) noexcept {
    if (ActiveHeapProbe != nullptr) {
        ActiveHeapProbe->Free(memory);
    }
    std::free(memory);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name): Standard-library names vary by implementation.
void operator delete(void* memory, std::size_t) noexcept {
    ::operator delete(memory);
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
#ifndef GRAPHICS_ENGINE_ECS_FAULT_ASAN
// ASan owns the delete interceptor in this executable; allocator instrumentation is
// exercised by normal Small/Medium profiles, while sanitizer profiles retain fault tests.
TEST(EcsRunFault, TrackingHeapIsBoundedAndPinnedSnapshotsReleaseTheirAllocations) {
    HeapProbe probe;
    std::size_t steady = 0;
    std::size_t pinned = 0;
    std::size_t released = 0;
    std::size_t maximumSteady = 0;
    std::size_t historyEpochs = 0;
    {
        HeapProbeScope scope(probe);
        {
            NCommon::ChangeTracker tracker({.MaxDomains = 4,
                                            .MaxHistoryEpochs = 4,
                                            .MaxHistoryRecords = 2048,
                                            .MaxRangesPerDomain = 64,
                                            .MaxChunkVersionsPerDomain = 64,
                                            .MaxEntityVersionsPerDomain = 64,
                                            .ChunkSize = 1});
            for (std::uint64_t type = 1; type <= 4; ++type) {
                tracker.RegisterDomain({1, type, 1}, true);
            }
            static_cast<void>(tracker.PublishEpoch());
            const auto publish = [&](std::uint64_t index) {
                NCommon::DirtySummary dirty;
                for (std::uint64_t type = 1; type <= 4; ++type) {
                    for (std::uint64_t item = 0; item < 32; ++item) {
                        dirty.MarkEntity({1, type, 1}, {(index % 2) * 64 + item * 2, 1});
                    }
                }
                const NCommon::DirtySummary* input = &dirty;
                static_cast<void>(tracker.PublishEpoch({&input, 1}));
            };
            for (std::uint64_t index = 0; index < 8; ++index) {
                publish(index);
            }
            steady = probe.LiveBytes;
            historyEpochs = tracker.CaptureSnapshot().GetStatistics().HistoryEpochs;
            std::array<NCommon::ChangeSnapshot, 8> snapshots;
            for (std::size_t index = 0; index < snapshots.size(); ++index) {
                publish(index);
                snapshots[index] = tracker.CaptureSnapshot();
            }
            pinned = probe.LiveBytes;
            snapshots = {};
            released = probe.LiveBytes;
            maximumSteady = released;
            for (std::uint64_t index = 0; index < 128; ++index) {
                publish(index);
                maximumSteady = std::max(maximumSteady, probe.LiveBytes);
            }
        }
    }
    ASSERT_FALSE(probe.Overflow);
    EXPECT_EQ(historyEpochs, 4U);
    EXPECT_GT(pinned, released);
    EXPECT_LE(released, steady + 4096);
    EXPECT_LE(maximumSteady, steady + 4096);
    EXPECT_LT(steady, 512U * 1024);
    EXPECT_LT(pinned, 2U * 1024 * 1024);
    EXPECT_LT(probe.PeakBytes, 4U * 1024 * 1024);
    EXPECT_EQ(probe.LiveBytes, 0U);
    std::cout << "ChangeTracker allocation-request bytes: steady=" << steady << " pinned=" << pinned
              << " released=" << released << " maxSteady=" << maximumSteady << " peak=" << probe.PeakBytes
              << " final=" << probe.LiveBytes << '\n';
}
#endif

TEST(EcsRunFault, TrackingPublicationAllocationFailurePreservesDirtyWrites) {
    NEcs::World world;
    const auto entity = world.CreateEntity();
    world.AddComponent<int>(entity, 0);
    const auto domain = NEcs::NInternal::WorldChangeAccess::Domain<int>(world);
    static_cast<void>(NEcs::NInternal::WorldChangeAccess::PublishEpoch(world));
    const auto cursor = NEcs::NInternal::WorldChangeAccess::CaptureSnapshot(world).GetCursor();
    world.RegisterSystem({NEcs::Write<int>()}, [entity](NEcs::World& owner) {
        ++owner.GetComponent<int>(entity);
        AllocationBudget = 0;
    });
    // Failure in finalization must not strand the World run guard or discard dirty evidence.
    EXPECT_NO_THROW(world.RunSystems());
    AllocationBudget = -1;
    const auto snapshot = NEcs::NInternal::WorldChangeAccess::CaptureSnapshot(world);
    EXPECT_EQ(std::as_const(world).GetComponent<int>(entity), 1);
    EXPECT_TRUE(snapshot.CaptureChanges(cursor, domain).FullDirty);
    EXPECT_FALSE(snapshot.GetStorageVersion(domain).IsValid());
    static_cast<void>(NEcs::NInternal::WorldChangeAccess::PublishEpoch(world));
    EXPECT_TRUE(NEcs::NInternal::WorldChangeAccess::CaptureSnapshot(world).CaptureChanges(cursor, domain).FullDirty);
}

TEST(EcsRunFault, EveryTrackingPublicationAllocationFailureKeepsTheOldRoot) {
    int failures = 0;
    int successes = 0;
    for (std::ptrdiff_t budget = 0; budget < 80; ++budget) {
        NCommon::ChangeTracker tracker;
        const NCommon::ChangeDomainKey domain{1, 1, 1};
        tracker.RegisterDomain(domain, true);
        static_cast<void>(tracker.PublishEpoch());
        const auto before = tracker.CaptureSnapshot();
        NCommon::DirtySummary dirty;
        dirty.MarkEntity(domain, {7, 1});
        const NCommon::DirtySummary* input = &dirty;
        bool failed = false;
        AllocationBudget = budget;
        try {
            static_cast<void>(tracker.PublishEpoch({&input, 1}));
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        AllocationBudget = -1;
        const auto after = tracker.CaptureSnapshot();
        if (failed) {
            ++failures;
            EXPECT_EQ(after.GetCursor(), before.GetCursor());
            EXPECT_TRUE(after.CaptureChanges(before.GetCursor(), domain).FullDirty);
            EXPECT_FALSE(after.GetStorageVersion(domain).IsValid());
            static_cast<void>(tracker.PublishEpoch());
            EXPECT_TRUE(tracker.CaptureChanges(before.GetCursor(), domain).FullDirty);
        } else {
            ++successes;
            EXPECT_EQ(after.CaptureChanges(before.GetCursor(), domain).Ranges,
                      (std::vector<NCommon::DirtyRange>{{7, 1}}));
        }
        EXPECT_FALSE(before.CaptureChanges(before.GetCursor(), domain).HasChanges());
    }
    EXPECT_GT(failures, 0);
    EXPECT_GT(successes, 0);
}

TEST(EcsRunFault, DeclaredLegacyWritesNeedNoPerWriteMetadataAllocation) {
    NEcs::World world;
    const auto entity = world.CreateEntity();
    world.AddComponent<int>(entity, 0);
    world.RegisterSystem({NEcs::Write<int>()}, [entity](NEcs::World& owner) {
        AllocationBudget = 0;
        for (int index = 0; index < 1000; ++index) {
            ++owner.GetComponent<int>(entity);
        }
        const auto remaining = AllocationBudget;
        AllocationBudget = -1;
        EXPECT_EQ(remaining, 0);
    });
    EXPECT_NO_THROW(world.RunSystems());
    EXPECT_EQ(std::as_const(world).GetComponent<int>(entity), 1000);
}

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

TEST(EcsRunFault, TraversalAdmissionFailuresReleaseTopologyAndCloseGroups) {
    int failures = 0;
    int successes = 0;
    NCommon::TaskSystem tasks{1};
    NEcs::NInternal::AdaptiveBatchPlanner planner{tasks};
    for (std::ptrdiff_t budget = 0; budget < 160; ++budget) {
        NEcs::World world;
        const auto root = world.GetEntityReference(world.CreateEntity());
        const std::vector roots{root};
        NEcs::NInternal::TraversalOperation<int> operation{
                .Visit = [](auto) { return 7; },
                .Output = std::make_shared<NEcs::NInternal::TraversalOutput<int>>(),
        };
        std::optional<NCommon::TaskGroupHandle> group;
        AllocationBudget = budget;
        try {
            group = planner.ScheduleTraversal(world.AcquireTopologySnapshot(),
                                              roots,
                                              NEcs::NInternal::ETraversalMode::TOP_DOWN,
                                              std::move(operation));
            AllocationBudget = -1;
            ++successes;
        } catch (const std::bad_alloc&) {
            AllocationBudget = -1;
            ++failures;
        }
        if (group) {
            tasks.WaitGroup(*group);
        }
        tasks.WaitIdle();
        EXPECT_NO_THROW(world.DestroyEntity(root.Identity));
    }
    EXPECT_GT(failures, 0);
    EXPECT_GT(successes, 0);
}

TEST(EcsRunFault, TraversalDynamicSpawnFailureDoesNotPublishResultsOrLeakLease) {
    NCommon::TaskSystem tasks{2};
    NEcs::NInternal::AdaptiveBatchPlanner planner{tasks};
    NEcs::World world;
    const auto root = world.GetEntityReference(world.CreateEntity());
    for (int index = 0; index < 8; ++index) {
        world.DeferReparent(world.GetEntityReference(world.CreateEntity()), root);
    }
    world.ApplyDeferredStructuralChanges();
    NEcs::NInternal::TraversalOperation<int> operation{
            .Visit =
                    [root](auto input) {
                        if (input.Entity == root) {
                            AllocationBudget = 0;
                        }
                        return 7;
                    },
            .Output = std::make_shared<NEcs::NInternal::TraversalOutput<int>>(),
    };
    const auto output = operation.Output;
    const auto group =
            planner.ScheduleTraversal(world.AcquireTopologySnapshot(),
                                      std::vector{root},
                                      NEcs::NInternal::ETraversalMode::TOP_DOWN,
                                      std::move(operation),
                                      NEcs::NInternal::BatchPolicy{.MinimumPacketTime = std::chrono::nanoseconds{0}});
    tasks.WaitGroup(group);
    EXPECT_EQ(tasks.GetGroupStatus(group), NCommon::ETaskGroupStatus::FAILED);
    EXPECT_FALSE(output->TryAcquire(tasks));
    EXPECT_NO_THROW(world.CreateEntity());
}
} // namespace
