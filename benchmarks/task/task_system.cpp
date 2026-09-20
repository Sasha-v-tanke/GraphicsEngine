#include <atomic>
#include <chrono>
#include <cstddef>
#include <vector>

#include <benchmark/benchmark.h>
#include <lib/thread/task/task_system.h>

namespace {

[[nodiscard]] std::size_t WorkerCount(benchmark::State& state) {
    return static_cast<std::size_t>(state.range(0));
}

[[nodiscard]] std::size_t TaskCount(benchmark::State& state) {
    return static_cast<std::size_t>(state.range(1));
}

void BM_TaskSystemIndependentThroughput(benchmark::State& state) {
    for ([[maybe_unused]] auto _: state) {
        NCommon::TaskSystem taskSystem{WorkerCount(state)};
        std::atomic<std::size_t> completed = 0;
        std::vector<NCommon::TaskHandle> tasks;
        tasks.reserve(TaskCount(state));

        const auto start = std::chrono::steady_clock::now();

        for (std::size_t index = 0; index < TaskCount(state); ++index) {
            tasks.push_back(taskSystem.Submit([&](NCommon::TaskContext&) { completed.fetch_add(1); }));
        }

        for (const NCommon::TaskHandle& task: tasks) {
            taskSystem.Wait(task);
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;

        benchmark::DoNotOptimize(completed.load());
        state.SetIterationTime(std::chrono::duration<double>(elapsed).count());
    }

    state.counters["tasks"] =
            benchmark::Counter(static_cast<double>(TaskCount(state)), benchmark::Counter::kIsIterationInvariantRate);
}

void BM_TaskSystemFanInLatency(benchmark::State& state) {
    for ([[maybe_unused]] auto _: state) {
        NCommon::TaskSystem taskSystem{WorkerCount(state)};
        std::atomic<std::size_t> prerequisitesDone = 0;
        std::atomic<bool> fanInCompleted = false;
        std::vector<NCommon::TaskHandle> prerequisites;
        prerequisites.reserve(TaskCount(state));

        for (std::size_t index = 0; index < TaskCount(state); ++index) {
            prerequisites.push_back(taskSystem.Submit([&](NCommon::TaskContext&) { prerequisitesDone.fetch_add(1); }));
        }

        const NCommon::TaskHandle fanIn =
                taskSystem.Submit([&](NCommon::TaskContext&) { fanInCompleted = true; }, prerequisites);

        const auto start = std::chrono::steady_clock::now();
        taskSystem.Wait(fanIn);
        const auto elapsed = std::chrono::steady_clock::now() - start;

        benchmark::DoNotOptimize(prerequisitesDone.load());
        benchmark::DoNotOptimize(fanInCompleted.load());
        state.SetIterationTime(std::chrono::duration<double>(elapsed).count());
    }

    state.counters["prerequisites"] =
            benchmark::Counter(static_cast<double>(TaskCount(state)), benchmark::Counter::kIsIterationInvariantRate);
}

} // namespace

BENCHMARK(BM_TaskSystemIndependentThroughput)
        ->Args({1, 1024})
        ->Args({2, 4096})
        ->Args({4, 8192})
        ->UseManualTime()
        ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_TaskSystemFanInLatency)
        ->Args({1, 256})
        ->Args({2, 1024})
        ->Args({4, 4096})
        ->UseManualTime()
        ->Unit(benchmark::kMicrosecond);
