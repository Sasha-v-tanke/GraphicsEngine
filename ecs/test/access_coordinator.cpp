#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <ecs/internal/access_coordinator.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <lib/thread/task/task_system.h>

namespace {
using NEcs::NInternal::AccessCoordinator;
using NEcs::NInternal::AccessFootprint;
using NEcs::NInternal::AccessScope;
using NEcs::NInternal::DataDomainKey;
using NEcs::NInternal::EAccessRequirement;
using NEcs::NInternal::EAccessScope;
using NEcs::NInternal::EOperationStatus;
using NEcs::NInternal::OperationKey;
using namespace std::chrono_literals;

constexpr DataDomainKey TEST_DOMAIN{1, 1, 1};

AccessFootprint Access(EAccessRequirement mode, AccessScope scope = {}) {
    return {TEST_DOMAIN, std::move(scope), mode};
}
AccessScope Entity(std::uint64_t index) {
    return {.Kind = EAccessScope::ENTITY, .Begin = index};
}

// The host guard opens even on fatal assertions; work retains its own shared state.
class HeldWork {
public:
    HeldWork()
        : m_state(std::make_shared<State>()) {
    }
    ~HeldWork() {
        Open();
    }
    std::function<void()> Function() {
        return [state = m_state] {
            state->Started.set_value();
            state->Release.wait();
        };
    }
    bool WaitStarted() {
        return m_state->Started.get_future().wait_for(2s) == std::future_status::ready;
    }
    void Open() {
        if (!m_open) {
            m_open = true;
            m_state->ReleasePromise.set_value();
        }
    }

private:
    struct State {
        std::promise<void> Started;
        std::promise<void> ReleasePromise;
        std::shared_future<void> Release = ReleasePromise.get_future().share();
    };
    std::shared_ptr<State> m_state;
    bool m_open = false;
};

void Register(AccessCoordinator& coordinator, DataDomainKey domain = TEST_DOMAIN) {
    coordinator.RegisterDomain(domain, std::make_shared<int>(1));
}

TEST(AccessCoordinator, CompatibilityMatrixAndConflictDiagnostics) {
    for (const auto firstMode: {EAccessRequirement::READ, EAccessRequirement::WRITE, EAccessRequirement::EXCLUSIVE}) {
        for (const auto secondMode:
             {EAccessRequirement::READ, EAccessRequirement::WRITE, EAccessRequirement::EXCLUSIVE}) {
            NCommon::TaskSystem tasks{2};
            AccessCoordinator coordinator{tasks};
            Register(coordinator);
            HeldWork first;
            const OperationKey earlier = coordinator.Schedule({Access(firstMode, Entity(7))}, first.Function());
            ASSERT_TRUE(first.WaitStarted());
            std::promise<void> laterStarted;
            const auto laterFuture = laterStarted.get_future();
            const OperationKey later =
                    coordinator.Schedule({Access(secondMode, Entity(7))}, [&] { laterStarted.set_value(); });
            const bool conflict = firstMode != EAccessRequirement::READ || secondMode != EAccessRequirement::READ;
            EXPECT_EQ(coordinator.GetDependencies(later).size(), conflict ? 1U : 0U);
            if (conflict) {
                EXPECT_EQ(coordinator.GetStatus(later), EOperationStatus::WAITING);
                const auto reasons = coordinator.GetConflicts(later);
                ASSERT_EQ(reasons.size(), 1U);
                EXPECT_EQ(reasons[0].Predecessor, earlier);
                EXPECT_EQ(reasons[0].Domain, TEST_DOMAIN);
            } else {
                EXPECT_EQ(laterFuture.wait_for(2s), std::future_status::ready);
            }
            first.Open();
            tasks.WaitIdle();
            EXPECT_EQ(coordinator.GetStatus(later), EOperationStatus::COMPLETED);
        }
    }
}

TEST(AccessCoordinator, NormalizesSetsAndResolvesChunkOverlapConservatively) {
    const std::vector<std::pair<AccessScope, bool>> scopes{
            {Entity(8), false},
            {Entity(2), true},
            {{.Kind = EAccessScope::ENTITY_SET, .Elements = {9, 4, 4, 1}}, true},
            {{.Kind = EAccessScope::ENTITY_SET, .Elements = {9, 8}}, false},
            {{.Kind = EAccessScope::CHUNK, .Begin = 5, .Count = 3}, false},
            {{.Kind = EAccessScope::CHUNK, .Begin = 4, .Count = 3}, true},
            {{.Kind = EAccessScope::WHOLE_STORAGE}, true},
            {{.Kind = EAccessScope::UNKNOWN}, true},
            {{.Kind = EAccessScope::RESOURCE, .Begin = 99}, true},
    };
    for (const auto& [scope, conflict]: scopes) {
        NCommon::TaskSystem tasks{1};
        AccessCoordinator coordinator{tasks};
        Register(coordinator);
        HeldWork first;
        (void)coordinator.Schedule(
                {Access(EAccessRequirement::WRITE, {.Kind = EAccessScope::CHUNK, .Begin = 2, .Count = 3})},
                first.Function());
        ASSERT_TRUE(first.WaitStarted());
        const OperationKey later = coordinator.Schedule({Access(EAccessRequirement::WRITE, scope)}, [] {});
        EXPECT_EQ(coordinator.GetDependencies(later).size(), conflict ? 1U : 0U);
        first.Open();
        tasks.WaitIdle();
    }
}

TEST(AccessCoordinator, DisjointScopesAndOwnersCanRunConcurrently) {
    NCommon::TaskSystem tasks{4};
    AccessCoordinator coordinator{tasks};
    Register(coordinator);
    Register(coordinator, {2, 1, 1});
    Register(coordinator, {1, 2, 1});
    HeldWork first;
    (void)coordinator.Schedule({Access(EAccessRequirement::WRITE, Entity(1))}, first.Function());
    ASSERT_TRUE(first.WaitStarted());
    std::array<OperationKey, 3> keys;
    std::array<std::promise<void>, 3> started;
    keys[0] = coordinator.Schedule({Access(EAccessRequirement::WRITE, Entity(2))}, [&] { started[0].set_value(); });
    keys[1] =
            coordinator.Schedule({{{2, 1, 1}, Entity(1), EAccessRequirement::WRITE}}, [&] { started[1].set_value(); });
    keys[2] =
            coordinator.Schedule({{{1, 2, 1}, Entity(1), EAccessRequirement::WRITE}}, [&] { started[2].set_value(); });
    for (std::size_t index = 0; index < keys.size(); ++index) {
        EXPECT_TRUE(coordinator.GetDependencies(keys[index]).empty());
        EXPECT_EQ(started[index].get_future().wait_for(2s), std::future_status::ready);
    }
    first.Open();
    tasks.WaitIdle();
}

TEST(AccessCoordinator, WholeStorageWriterCannotBeOvertakenByLaterReaders) {
    for (const std::size_t workers: {1U, 2U, 4U}) {
        NCommon::TaskSystem tasks{workers};
        AccessCoordinator coordinator{tasks};
        Register(coordinator);
        HeldWork first;
        const OperationKey reader =
                coordinator.Schedule({Access(EAccessRequirement::READ, Entity(4))}, first.Function());
        ASSERT_TRUE(first.WaitStarted());
        std::atomic<int> value{0};
        const OperationKey writer = coordinator.Schedule({Access(EAccessRequirement::WRITE)}, [&] { value.store(42); });
        for (int count = 0; count < 64; ++count) {
            const OperationKey later =
                    coordinator.Schedule({Access(EAccessRequirement::READ, Entity(5))}, [&] { EXPECT_EQ(value, 42); });
            EXPECT_EQ(coordinator.GetDependencies(later), std::vector<OperationKey>{writer});
        }
        EXPECT_EQ(coordinator.GetDependencies(writer), std::vector<OperationKey>{reader});
        first.Open();
        tasks.WaitIdle();
        EXPECT_EQ(coordinator.GetStatus(writer), EOperationStatus::COMPLETED);
    }
}

TEST(AccessCoordinator, WaitingOperationsDoNotPublishPollingOrBlockingTasks) {
    NCommon::TaskSystem tasks{1};
    AccessCoordinator coordinator{tasks};
    Register(coordinator);
    HeldWork first;
    const OperationKey running = coordinator.Schedule({Access(EAccessRequirement::WRITE)}, first.Function());
    ASSERT_TRUE(first.WaitStarted());
    for (int count = 0; count < 32; ++count) {
        (void)coordinator.Schedule({Access(EAccessRequirement::WRITE)}, [] {});
    }
    const auto marker = tasks.Submit([](NCommon::TaskContext&) {});
    EXPECT_EQ(marker.GetId(), 2U); // Only the running operation has entered TaskSystem.
    EXPECT_FALSE(coordinator.Cancel(running));
    first.Open();
    tasks.WaitIdle();
}

TEST(AccessCoordinator, PendingCancellationAndFailureCancelDiamondWithoutWorkerWait) {
    for (const bool shouldFail: {false, true}) {
        NCommon::TaskSystem tasks{1};
        AccessCoordinator coordinator{tasks};
        Register(coordinator);
        HeldWork first;
        const auto heldFunction = first.Function();
        const OperationKey root = coordinator.Schedule({}, [&, heldFunction] {
            heldFunction();
            if (shouldFail) {
                throw std::runtime_error("operation failed");
            }
        });
        ASSERT_TRUE(first.WaitStarted());
        int callbacks = 0;
        const std::array rootDependency{root};
        const OperationKey left = coordinator.Schedule({}, [&] { ++callbacks; }, rootDependency);
        const std::array both{root, left};
        const OperationKey join = coordinator.Schedule({}, [&] { ++callbacks; }, both);
        if (!shouldFail) {
            EXPECT_TRUE(coordinator.Cancel(left));
            EXPECT_FALSE(coordinator.Cancel(left));
        }
        first.Open();
        tasks.WaitIdle();
        EXPECT_EQ(callbacks, 0);
        EXPECT_EQ(coordinator.GetStatus(join), EOperationStatus::CANCELLED);
        if (shouldFail) {
            EXPECT_EQ(coordinator.GetStatus(root), EOperationStatus::FAILED);
            EXPECT_NE(coordinator.GetError(root), nullptr);
            EXPECT_THROW((void)coordinator.Schedule({}, [] {}), NCommon::Exception);
        }
    }
}

TEST(AccessCoordinator, DomainGenerationCannotChangeWhileActiveAndOldScopeIsRejected) {
    NCommon::TaskSystem tasks{1};
    AccessCoordinator coordinator{tasks};
    Register(coordinator);
    HeldWork first;
    (void)coordinator.Schedule({Access(EAccessRequirement::WRITE)}, first.Function());
    ASSERT_TRUE(first.WaitStarted());
    EXPECT_THROW(Register(coordinator, {1, 1, 2}), NCommon::Exception);
    first.Open();
    tasks.WaitIdle();
    Register(coordinator, {1, 1, 2});
    EXPECT_THROW((void)coordinator.Schedule({Access(EAccessRequirement::READ)}, [] {}), NCommon::Exception);
    EXPECT_THROW(Register(coordinator, TEST_DOMAIN), NCommon::Exception);
    const OperationKey fresh = coordinator.Schedule({{{1, 1, 2}, Entity(1), EAccessRequirement::WRITE}}, [] {});
    tasks.WaitIdle();
    EXPECT_EQ(coordinator.GetStatus(fresh), EOperationStatus::COMPLETED);
}

TEST(AccessCoordinator, RejectsForeignKeysAndMalformedScopesWithoutConsumingSequence) {
    NCommon::TaskSystem tasks{1};
    AccessCoordinator coordinator{tasks};
    AccessCoordinator other{tasks};
    Register(coordinator);
    const OperationKey foreign = other.Schedule({}, [] {});
    const std::array dependencies{foreign};
    EXPECT_THROW((void)coordinator.Schedule({}, [] {}, dependencies), NCommon::Exception);
    EXPECT_THROW((void)coordinator.GetStatus({}), NCommon::Exception);
    for (const AccessScope& invalid:
         std::vector<AccessScope>{{.Kind = EAccessScope::ENTITY_SET},
                                  {.Kind = EAccessScope::CHUNK, .Begin = UINT64_MAX, .Count = 2},
                                  {.Kind = EAccessScope::ENTITY, .Count = 2},
                                  {.Kind = static_cast<EAccessScope>(99)}}) {
        EXPECT_THROW((void)coordinator.Schedule({Access(EAccessRequirement::WRITE, invalid)}, [] {}),
                     NCommon::Exception);
    }
    const OperationKey valid = coordinator.Schedule({}, [] {});
    EXPECT_EQ(valid.GetSequence(), 1U);
    tasks.WaitIdle();
}

TEST(AccessCoordinator, StopKeepsRunningAccessAndOwnerLeaseUntilDrained) {
    NCommon::TaskSystem tasks{1};
    AccessCoordinator coordinator{tasks};
    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<int> weak = lifetime;
    coordinator.RegisterDomain(TEST_DOMAIN, lifetime);
    lifetime.reset();
    HeldWork first;
    const OperationKey running = coordinator.Schedule({Access(EAccessRequirement::WRITE)}, first.Function());
    ASSERT_TRUE(first.WaitStarted());
    const OperationKey pending = coordinator.Schedule({Access(EAccessRequirement::WRITE)}, [] { FAIL(); });
    auto stop = std::async(std::launch::async, [&] { coordinator.Stop(); });
    EXPECT_EQ(stop.wait_for(20ms), std::future_status::timeout);
    EXPECT_FALSE(weak.expired());
    first.Open();
    ASSERT_EQ(stop.wait_for(2s), std::future_status::ready);
    stop.get();
    EXPECT_EQ(coordinator.GetStatus(running), EOperationStatus::COMPLETED);
    EXPECT_EQ(coordinator.GetStatus(pending), EOperationStatus::CANCELLED);
    EXPECT_THROW((void)coordinator.Schedule({}, [] {}), NCommon::Exception);
    coordinator.Stop();
    EXPECT_TRUE(weak.expired());
}

TEST(AccessCoordinator, QueuedCancellationSkipsCallbackAndCaptureDestructorsCanReenter) {
    NCommon::TaskSystem tasks{1};
    AccessCoordinator coordinator{tasks};
    HeldWork blocker;
    const auto heldFunction = blocker.Function();
    const auto hold = tasks.Submit([heldFunction](NCommon::TaskContext&) { heldFunction(); });
    ASSERT_TRUE(blocker.WaitStarted());
    int destroyed = 0;
    struct Capture {
        AccessCoordinator& Coordinator;
        int& Destroyed;
        ~Capture() {
            ++Destroyed;
            (void)Coordinator.Schedule({}, [] {});
        }
    };
    auto capture = std::make_shared<Capture>(coordinator, destroyed);
    const OperationKey pending = coordinator.Schedule({}, [capture] { FAIL(); });
    capture.reset();
    EXPECT_TRUE(coordinator.Cancel(pending));
    EXPECT_EQ(destroyed, 1);
    blocker.Open();
    tasks.Wait(hold);
    tasks.WaitIdle();
}
TEST(AccessCoordinator, ResourceScopesAndCanonicalSetsHaveExactConflicts) {
    for (const bool resources: {false, true}) {
        NCommon::TaskSystem tasks{1};
        AccessCoordinator coordinator{tasks};
        Register(coordinator);
        HeldWork first;
        const AccessScope left = resources ? AccessScope{.Kind = EAccessScope::RESOURCE, .Begin = 8}
                                           : AccessScope{.Kind = EAccessScope::ENTITY_SET, .Elements = {8, 2, 8}};
        const OperationKey predecessor =
                coordinator.Schedule({Access(EAccessRequirement::WRITE, left)}, first.Function());
        ASSERT_TRUE(first.WaitStarted());
        const AccessScope matching =
                resources ? left : AccessScope{.Kind = EAccessScope::ENTITY_SET, .Elements = {9, 8}};
        const AccessScope disjoint = resources ? AccessScope{.Kind = EAccessScope::RESOURCE, .Begin = 9}
                                               : AccessScope{.Kind = EAccessScope::ENTITY_SET, .Elements = {9, 3}};
        const OperationKey conflict = coordinator.Schedule({Access(EAccessRequirement::WRITE, matching)}, [] {});
        const OperationKey independent = coordinator.Schedule({Access(EAccessRequirement::READ, disjoint)}, [] {});
        EXPECT_EQ(coordinator.GetDependencies(conflict), std::vector<OperationKey>{predecessor});
        // The disjoint operation may still depend on matching when they share 9.
        const auto reasons = coordinator.GetConflicts(independent);
        EXPECT_TRUE(
                std::ranges::none_of(reasons, [&](const auto& reason) { return reason.Predecessor == predecessor; }));
        first.Open();
        tasks.WaitIdle();
    }
}

TEST(AccessCoordinator, WorkerCanScheduleConflictingContinuationWithOneWorker) {
    NCommon::TaskSystem tasks{1};
    AccessCoordinator coordinator{tasks};
    Register(coordinator);
    std::atomic<int> calls{0};
    OperationKey child;
    const OperationKey parent = coordinator.Schedule({Access(EAccessRequirement::WRITE)}, [&] {
        calls = 1;
        child = coordinator.Schedule({Access(EAccessRequirement::WRITE)}, [&] {
            EXPECT_EQ(calls, 1);
            calls = 2;
        });
    });
    tasks.WaitIdle();
    EXPECT_EQ(coordinator.GetStatus(parent), EOperationStatus::COMPLETED);
    EXPECT_EQ(coordinator.GetStatus(child), EOperationStatus::COMPLETED);
    EXPECT_EQ(calls, 2);
}

TEST(AccessCoordinator, ParallelProducersSerializeSameRegionAndKeepIndependentRegionsCorrect) {
    for (const std::size_t workers: {1U, 4U}) {
        NCommon::TaskSystem tasks{workers};
        AccessCoordinator coordinator{tasks};
        Register(coordinator);
        std::array<int, 8> values{};
        std::vector<std::jthread> producers;
        producers.reserve(4);
        for (std::size_t producer = 0; producer < 4; ++producer) {
            producers.emplace_back([&, producer] {
                for (std::size_t iteration = 0; iteration < 64; ++iteration) {
                    const std::size_t region = (producer + iteration) % values.size();
                    (void)coordinator.Schedule({Access(EAccessRequirement::WRITE, Entity(region))},
                                               [&, region] { ++values[region]; });
                }
            });
        }
        producers.clear();
        tasks.WaitIdle();
        for (const int value: values) {
            EXPECT_EQ(value, 32);
        }
    }
}
} // namespace
