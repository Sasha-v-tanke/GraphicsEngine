#include <benchmark/benchmark.h>

namespace {

void BM_Smoke(benchmark::State& state) {
    for ([[maybe_unused]] auto _: state) {
        benchmark::DoNotOptimize(state.iterations());
    }
}

BENCHMARK(BM_Smoke);

} // namespace
