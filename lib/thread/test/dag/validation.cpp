#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <lib/thread/task/internal/task_state.h>

#ifndef NDEBUG
namespace NCommon {

struct TaskSystemTestAccess {
    enum class EFault {
        REMAINING,
        INCOMING_HEAD,
        OUTGOING_HEAD,
        INCOMING_PREVIOUS,
        DANGLING_HEAD,
        DANGLING_DEPENDENT,
        DUPLICATE_PARENT,
        DEGREE,
        IDENTITY,
        STATUS,
    };

    static const char* Full(TaskSystem& pool) {
        std::lock_guard lock{pool.m_mutex};
        return pool.FindDagViolationFullLocked();
    }
    static void Audit(TaskSystem& pool) {
        std::lock_guard lock{pool.m_mutex};
        pool.ValidateDagFullLocked();
    }
    static auto Stats(TaskSystem& pool) {
        std::lock_guard lock{pool.m_mutex};
        return pool.GetDagValidationStatsLocked();
    }

    static auto Probe(TaskSystem& pool, const TaskHandle& child, EFault fault) {
        std::lock_guard lock{pool.m_mutex};
        auto& state = *child.m_state;
        auto& task = state.Task;
        auto& first = task.Dependencies[0];
        auto& second = task.Dependencies[1];
        const auto savedFirst = first;
        const auto savedSecond = second;
        auto& left = first.Prerequisite->Task;
        auto& right = second.Prerequisite->Task;
        const auto savedIncoming = task.Incoming;
        const auto savedLeftOutgoing = left.Outgoing;
        const auto savedRightOutgoing = right.Outgoing;
        const std::size_t savedDegree = task.DebugIncomingCount;
        const std::size_t savedLeftDegree = left.DebugOutgoingCount;
        const std::size_t savedRightDegree = right.DebugOutgoingCount;
        const std::size_t savedRemaining = task.RemainingDependencies.load();
        const auto savedId = state.Id;
        const auto savedStatus = task.Status;
        // Restore before releasing G, including when an explicit full audit fails to allocate.
        const auto restore = [&] {
            first = savedFirst;
            second = savedSecond;
            task.Incoming = savedIncoming;
            left.Outgoing = savedLeftOutgoing;
            right.Outgoing = savedRightOutgoing;
            task.DebugIncomingCount = savedDegree;
            left.DebugOutgoingCount = savedLeftDegree;
            right.DebugOutgoingCount = savedRightDegree;
            task.RemainingDependencies.store(savedRemaining);
            state.Id = savedId;
            task.Status = savedStatus;
        };
        struct Restore {
            const decltype(restore)& Function;
            ~Restore() {
                Function();
            }
        } guard{restore};
        bool inspectLocal = true;
        switch (fault) {
        case EFault::REMAINING:
            task.RemainingDependencies.fetch_add(1);
            break;
        case EFault::INCOMING_HEAD:
            task.Incoming = nullptr;
            break;
        case EFault::OUTGOING_HEAD:
            left.Outgoing = nullptr;
            inspectLocal = false;
            break;
        case EFault::INCOMING_PREVIOUS:
            task.Incoming->PreviousIncoming = &first;
            break;
        case EFault::DANGLING_HEAD:
            // The full audit must reject an unknown address before dereferencing it.
            task.Incoming = reinterpret_cast<TaskSystem::DependencyEdge*>(std::uintptr_t{0x1230});
            inspectLocal = false;
            break;
        case EFault::DANGLING_DEPENDENT:
            first.Dependent = reinterpret_cast<TaskHandle::State*>(std::uintptr_t{0x1230});
            inspectLocal = false;
            break;
        case EFault::DUPLICATE_PARENT:
            // Preserve both chains and their degrees; only the prerequisite uniqueness is wrong.
            left.Outgoing = nullptr;
            left.DebugOutgoingCount = 0;
            right.DebugOutgoingCount = 2;
            first.Prerequisite = second.Prerequisite;
            first.PreviousOutgoing = &second;
            first.NextOutgoing = nullptr;
            second.NextOutgoing = &first;
            inspectLocal = false;
            break;
        case EFault::DEGREE:
            task.DebugIncomingCount = 0;
            break;
        case EFault::IDENTITY:
            state.Id = 0;
            break;
        case EFault::STATUS:
            task.Status = ETaskStatus::CREATED;
            break;
        }
        const char* full = pool.FindDagViolationFullLocked();
        const char* local = inspectLocal ? TaskSystem::FindMutationViolationLocked(state) : nullptr;
        return std::pair{full, local};
    }
};

} // namespace NCommon

namespace {

class PausedPool final {
public:
    explicit PausedPool(std::size_t workers = 1)
        : Pool(workers) {
        for (std::size_t index = 0; index < workers; ++index) {
            Pool.Submit([this](NCommon::TaskContext&) {
                m_entered.release();
                m_resume.acquire();
            });
        }
        for (std::size_t index = 0; index < workers; ++index) {
            m_entered.acquire();
        }
    }
    ~PausedPool() {
        m_resume.release(static_cast<std::ptrdiff_t>(Pool.GetWorkerCount()));
        Pool.WaitIdle();
    }

private:
    std::counting_semaphore<> m_entered{0};
    std::counting_semaphore<> m_resume{0};

public:
    NCommon::TaskSystem Pool;
};

TEST(DebugDag, FullAndLocalChecksRejectCorruptionAndRestoreTheGraph) {
    PausedPool fixture;
    auto& pool = fixture.Pool;
    const NCommon::TaskHandle parents[]{pool.Submit([](NCommon::TaskContext&) {}),
                                        pool.Submit([](NCommon::TaskContext&) {})};
    const auto child = pool.Submit([](NCommon::TaskContext&) {}, parents);
    using Access = NCommon::TaskSystemTestAccess;
    using Fault = Access::EFault;
    for (const auto fault: {Fault::REMAINING,
                            Fault::INCOMING_HEAD,
                            Fault::OUTGOING_HEAD,
                            Fault::INCOMING_PREVIOUS,
                            Fault::DANGLING_HEAD,
                            Fault::DANGLING_DEPENDENT,
                            Fault::DUPLICATE_PARENT,
                            Fault::DEGREE,
                            Fault::IDENTITY,
                            Fault::STATUS}) {
        ASSERT_EQ(Access::Full(pool), nullptr);
        const auto [full, local] = Access::Probe(pool, child, fault);
        ASSERT_NE(full, nullptr) << static_cast<int>(fault);
        if (fault == Fault::REMAINING || fault == Fault::INCOMING_HEAD || fault == Fault::INCOMING_PREVIOUS ||
            fault == Fault::DEGREE || fault == Fault::IDENTITY || fault == Fault::STATUS) {
            EXPECT_NE(local, nullptr) << static_cast<int>(fault);
        }
        if (fault == Fault::DUPLICATE_PARENT) {
            EXPECT_NE(std::string(full).find("duplicate"), std::string::npos);
        }
        Access::Audit(pool);
    }
    pool.Cancel(parents[0]);
    Access::Audit(pool);
}

TEST(DebugDag, ExplicitAuditAcceptsSharedDiamondsAndConcurrentCompletion) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        NCommon::TaskSystem pool{workers};
        for (std::size_t iteration = 0; iteration < 64; ++iteration) {
            const auto root = pool.Submit([](NCommon::TaskContext&) {});
            const auto left = pool.Submit([](NCommon::TaskContext&) {}, std::span{&root, 1});
            const NCommon::TaskHandle parents[]{root, left};
            const auto right = pool.Submit([](NCommon::TaskContext&) {}, parents);
            NCommon::TaskSystemTestAccess::Audit(pool);
            pool.Wait(right);
            NCommon::TaskSystemTestAccess::Audit(pool);
        }
        pool.WaitIdle();
        NCommon::TaskSystemTestAccess::Audit(pool);
    }
}

struct PausedDestruction {
    std::binary_semaphore* Entered = nullptr;
    std::binary_semaphore* Resume = nullptr;
    ~PausedDestruction() {
        Entered->release();
        Resume->acquire();
    }
};

TEST(DebugDag, ExplicitAuditAcceptsFinalizingSuccessFailureAndGroupGates) {
    for (const bool fail: {false, true}) {
        std::binary_semaphore start{0};
        std::binary_semaphore entered{0};
        std::binary_semaphore resume{0};
        NCommon::TaskSystem pool{2};
        auto probe = std::make_shared<PausedDestruction>();
        probe->Entered = &entered;
        probe->Resume = &resume;
        const auto root = pool.Submit([probe = std::move(probe), &start, fail](NCommon::TaskContext&) {
            start.acquire();
            if (fail) {
                throw 48;
            }
        });
        const auto first = pool.Submit([](NCommon::TaskContext&) {}, std::span{&root, 1});
        start.release();
        entered.acquire();
        // Ensure even an unexpected audit allocation exception cannot strand the paused destructor.
        struct ResumeOnExit {
            std::binary_semaphore& Resume;
            bool Released = false;
            void Release() {
                if (!Released) {
                    Released = true;
                    Resume.release();
                }
            }
            ~ResumeOnExit() {
                Release();
            }
        } release{resume};
        NCommon::TaskSystemTestAccess::Audit(pool);
        const auto late = pool.Submit([](NCommon::TaskContext&) {}, std::span{&root, 1});
        NCommon::TaskSystemTestAccess::Audit(pool);
        EXPECT_EQ(pool.GetStatus(root), NCommon::ETaskStatus::RUNNING);
        release.Release();
        pool.WaitIdle();
        NCommon::TaskSystemTestAccess::Audit(pool);
        EXPECT_EQ(pool.GetStatus(first), fail ? NCommon::ETaskStatus::CANCELLED : NCommon::ETaskStatus::COMPLETED);
        EXPECT_EQ(pool.GetStatus(late), fail ? NCommon::ETaskStatus::CANCELLED : NCommon::ETaskStatus::COMPLETED);
    }
    NCommon::TaskSystem pool{2};
    const auto group = pool.CreateTaskGroup();
    const auto continuation = pool.SubmitAfterGroup(group, [](NCommon::TaskContext&) {});
    NCommon::TaskSystemTestAccess::Audit(pool);
    pool.SealGroup(group);
    NCommon::TaskSystemTestAccess::Audit(pool);
    pool.WaitIdle();
    EXPECT_EQ(pool.GetStatus(continuation), NCommon::ETaskStatus::COMPLETED);
    NCommon::TaskSystemTestAccess::Audit(pool);
}

TEST(DebugDag, MutationVisitsStayLinearForChainsFanInAndFanOut) {
    for (const std::size_t count: {1000U, 4000U, 16000U}) {
        for (const std::size_t shape: {0U, 1U, 2U}) {
            PausedPool fixture;
            auto& pool = fixture.Pool;
            const auto before = NCommon::TaskSystemTestAccess::Stats(pool);
            std::vector<NCommon::TaskHandle> tasks;
            tasks.reserve(count);
            tasks.push_back(pool.Submit([](NCommon::TaskContext&) {}));
            for (std::size_t index = 1; index < count; ++index) {
                const auto& parent = shape == 0 ? tasks.back() : tasks.front();
                tasks.push_back(pool.Submit([](NCommon::TaskContext&) {}, std::span{&parent, 1}));
            }
            if (shape == 2) {
                tasks.push_back(pool.Submit([](NCommon::TaskContext&) {}, tasks));
            }
            const auto admitted = NCommon::TaskSystemTestAccess::Stats(pool);
            EXPECT_EQ(admitted.FullStates, before.FullStates);
            EXPECT_EQ(admitted.FullEdges, before.FullEdges);
            EXPECT_LT(admitted.MutationStates - before.MutationStates, 3 * count);
            EXPECT_LT(admitted.MutationEdges - before.MutationEdges, 3 * count);
            NCommon::TaskSystemTestAccess::Audit(pool);
            const auto audited = NCommon::TaskSystemTestAccess::Stats(pool);
            pool.Cancel(tasks.front());
            const auto cancelled = NCommon::TaskSystemTestAccess::Stats(pool);
            EXPECT_LT(cancelled.MutationStates - audited.MutationStates, 4 * count);
            EXPECT_LT(cancelled.MutationEdges - audited.MutationEdges, 3 * count);
            EXPECT_EQ(cancelled.FullStates, audited.FullStates);
            EXPECT_EQ(cancelled.FullEdges, audited.FullEdges);
            NCommon::TaskSystemTestAccess::Audit(pool);
            RecordProperty("admit_visits_" + std::to_string(count) + "_" + std::to_string(shape),
                           std::to_string(admitted.MutationStates - before.MutationStates + admitted.MutationEdges -
                                          before.MutationEdges));
        }
    }
}

} // namespace
#endif
