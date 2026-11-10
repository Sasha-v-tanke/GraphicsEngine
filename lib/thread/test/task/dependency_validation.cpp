#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <random>
#include <semaphore>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <lib/thread/task/task_system.h>

namespace NCommon {

struct TaskSystemTestAccess {
    static std::size_t CompareDependencies(TaskSystem& pool, std::span<const TaskHandle> dependencies) {
        std::lock_guard lock{pool.m_mutex};
        std::size_t comparisons = 0;
        pool.m_registry.ValidateDependenciesMeasuredLocked(dependencies, comparisons);
        return comparisons;
    }
};

} // namespace NCommon

namespace {

std::vector<NCommon::TaskHandle> CompletedParents(NCommon::TaskSystem& pool, std::size_t count) {
    std::vector<NCommon::TaskHandle> parents;
    parents.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto task = pool.Submit([](NCommon::TaskContext&) {});
        pool.Wait(task);
        parents.push_back(task);
    }
    pool.WaitIdle();
    return parents;
}

void ExpectInvalidDependencies(NCommon::TaskSystem& pool, std::span<const NCommon::TaskHandle> dependencies) {
    try {
        pool.Submit([](NCommon::TaskContext&) {}, dependencies);
        ADD_FAILURE() << "Invalid dependencies were accepted";
    } catch (const NCommon::Exception& error) {
        EXPECT_EQ(error.code(), NCommon::make_error_code(NCommon::EError::INVALID_ARGUMENT));
    }
}

TEST(TaskSystem, DependencyValidationScalesWithoutReorderingHandles) {
    NCommon::TaskSystem pool{2};
    auto parents = CompletedParents(pool, 10000);
    // NOLINTNEXTLINE(cert-msc51-cpp): A fixed permutation makes regression and baseline inputs reproducible.
    std::mt19937 random{47};
    std::ranges::shuffle(parents, random);
    const auto original = parents;
    for (const std::size_t count: {0U, 1U, 4U, 16U, 17U, 128U, 1024U, 4096U, 10000U}) {
        const std::span dependencies{parents.data(), count};
        const std::size_t comparisons = NCommon::TaskSystemTestAccess::CompareDependencies(pool, dependencies);
        RecordProperty("comparisons_" + std::to_string(count), std::to_string(comparisons));
        if (count <= 16) {
            EXPECT_EQ(comparisons, count == 0 ? 0 : count * (count - 1) / 2);
        } else {
            EXPECT_LE(comparisons, 4 * count * static_cast<std::size_t>(std::bit_width(count)));
            if (count >= 128) {
                EXPECT_LT(comparisons, count * (count - 1) / 4);
            }
        }
        const auto task = pool.Submit([](NCommon::TaskContext&) {}, dependencies);
        pool.Wait(task);
        EXPECT_EQ(pool.GetStatus(task), NCommon::ETaskStatus::COMPLETED);
        EXPECT_EQ(parents, original);
    }
}

TEST(TaskSystem, DependencyValidationRejectsInvalidForeignAndDuplicateHandles) {
    NCommon::TaskSystem pool{2};
    NCommon::TaskSystem foreignPool{1};
    const auto foreign = foreignPool.Submit([](NCommon::TaskContext&) {});
    const auto valid = CompletedParents(pool, 10000);
    NCommon::TaskHandle expiredOwner;
    {
        NCommon::TaskSystem oldPool{1};
        expiredOwner = oldPool.Submit([](NCommon::TaskContext&) {});
        oldPool.Wait(expiredOwner);
    }
    for (const std::size_t count: {4U, 16U, 17U, 128U, 1024U, 4096U, 10000U}) {
        std::vector<NCommon::TaskHandle> parents(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(count));
        for (const auto& invalid: {NCommon::TaskHandle{}, foreign, expiredOwner, parents.front()}) {
            parents.back() = invalid;
            ExpectInvalidDependencies(pool, parents);
        }
        parents.back() = valid[count - 1];
        const auto task = pool.Submit([](NCommon::TaskContext&) {}, parents);
        pool.Wait(task);
        EXPECT_EQ(pool.GetStatus(task), NCommon::ETaskStatus::COMPLETED);
    }
}

TEST(TaskSystem, LargeDependencyValidationPreservesFailedParentPolicy) {
    NCommon::TaskSystem pool{2};
    auto parents = CompletedParents(pool, 128);
    const auto failed = pool.Submit([](NCommon::TaskContext&) { throw 47; });
    pool.Wait(failed);
    parents[64] = failed;
    std::atomic_bool invoked = false;
    const auto task = pool.Submit([&](NCommon::TaskContext&) { invoked.store(true); }, parents);
    pool.Wait(task);
    EXPECT_EQ(pool.GetStatus(task), NCommon::ETaskStatus::CANCELLED);
    EXPECT_FALSE(invoked.load());
}

TEST(TaskSystem, LargeFanInAcceptsConcurrentPrerequisiteCompletion) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        std::binary_semaphore entered{0};
        std::binary_semaphore resume{0};
        NCommon::TaskSystem pool{workers};
        std::atomic_size_t completed = 0;
        const auto root = pool.Submit([&](NCommon::TaskContext&) {
            entered.release();
            resume.acquire();
        });
        entered.acquire();
        std::vector<NCommon::TaskHandle> parents;
        parents.reserve(1024);
        for (std::size_t index = 0; index < 1024; ++index) {
            parents.push_back(pool.Submit([&](NCommon::TaskContext&) { completed.fetch_add(1); }, std::span{&root, 1}));
        }
        resume.release();
        const auto join =
                pool.Submit([&](NCommon::TaskContext&) { EXPECT_EQ(completed.load(), parents.size()); }, parents);
        pool.Wait(join);
        pool.WaitIdle();
        EXPECT_EQ(pool.GetStatus(join), NCommon::ETaskStatus::COMPLETED);
    }
}

} // namespace
