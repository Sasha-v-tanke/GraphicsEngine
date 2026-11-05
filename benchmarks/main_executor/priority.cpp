#include <algorithm>
#include <chrono>
#include <vector>

#include <application/runtime/main_executor.h>
#include <benchmark/benchmark.h>

static void MainCommandLatency(benchmark::State& state) {
    using Clock = std::chrono::steady_clock;
    std::vector<double> delays;
    for ([[maybe_unused]] auto iteration: state) {
        NApplication::NRuntime::MainExecutor executor;
        const auto generation = executor.Start();
        for (int index = 0; index < 128; ++index) {
            const auto queued = Clock::now();
            const NCommon::FrameOriginScope origin{
                    index % 4 == 0 ? std::nullopt
                                   : std::optional<NCommon::FrameOrigin>{{1, static_cast<std::uint64_t>(128 - index)}}};
            (void)executor.Post(generation, [&, queued] {
                delays.push_back(std::chrono::duration<double, std::micro>(Clock::now() - queued).count());
            });
        }
        while (executor.GetPendingCount() != 0) {
            executor.Drain();
        }
    }
    std::sort(delays.begin(), delays.end());
    if (!delays.empty()) {
        state.counters["queue_p95_us"] = delays[delays.size() * 95 / 100];
        state.counters["queue_p99_us"] = delays[delays.size() * 99 / 100];
    }
}
BENCHMARK(MainCommandLatency);
