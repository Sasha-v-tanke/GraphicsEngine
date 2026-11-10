#include <algorithm>
#include <chrono>
#include <cstddef>
#include <random>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>
#include <lib/thread/task/task_system.h>

namespace NCommon {

struct TaskSystemTestAccess {
    static void ValidateDependencies(TaskSystem& pool, std::span<const TaskHandle> dependencies) {
        std::lock_guard lock{pool.m_mutex};
        pool.m_registry.ValidateDependenciesLocked(dependencies);
    }
};

} // namespace NCommon

namespace {

std::vector<NCommon::TaskHandle> MakeCompletedDependencies(NCommon::TaskSystem& pool, std::size_t count) {
    std::vector<NCommon::TaskHandle> parents;
    parents.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto task = pool.Submit([](NCommon::TaskContext&) {});
        pool.Wait(task);
        parents.push_back(task);
    }
    pool.WaitIdle();
    // NOLINTNEXTLINE(cert-msc51-cpp): A fixed permutation makes regression and baseline inputs reproducible.
    std::mt19937 random{47};
    std::ranges::shuffle(parents, random);
    return parents;
}

void BM_TaskSystemDependencyValidation(benchmark::State& state) {
    NCommon::TaskSystem pool{1};
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto parents = MakeCompletedDependencies(pool, count);
    for ([[maybe_unused]] auto _: state) {
        NCommon::TaskSystemTestAccess::ValidateDependencies(pool, parents);
        benchmark::ClobberMemory();
    }
    state.counters["dependencies"] = static_cast<double>(count);
}

void BM_TaskSystemDependencySubmitLatency(benchmark::State& state) {
    NCommon::TaskSystem pool{1};
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto parents = MakeCompletedDependencies(pool, count);
    for ([[maybe_unused]] auto _: state) {
        const auto start = std::chrono::steady_clock::now();
        const auto task = pool.Submit([](NCommon::TaskContext&) {}, parents);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        // Only Submit is timed; execution/Wait latency does not hide validation costs.
        pool.Wait(task);
        state.SetIterationTime(std::chrono::duration<double>(elapsed).count());
    }
    state.counters["dependencies"] = static_cast<double>(count);
}

} // namespace

BENCHMARK(BM_TaskSystemDependencyValidation)
        ->Arg(0)
        ->Arg(1)
        ->Arg(4)
        ->Arg(16)
        ->Arg(128)
        ->Arg(1024)
        ->Arg(4096)
        ->Arg(10000);
BENCHMARK(BM_TaskSystemDependencySubmitLatency)
        ->Arg(0)
        ->Arg(1)
        ->Arg(4)
        ->Arg(16)
        ->Arg(128)
        ->Arg(1024)
        ->Arg(4096)
        ->Arg(10000)
        ->UseManualTime();
