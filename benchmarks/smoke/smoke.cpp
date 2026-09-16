#include <benchmark/benchmark.h>

static void BM_Smoke(benchmark::State& state) {
    for ([[maybe_unused]] auto _: state) {
        benchmark::DoNotOptimize(state.iterations());
    }
}

BENCHMARK(BM_Smoke);
