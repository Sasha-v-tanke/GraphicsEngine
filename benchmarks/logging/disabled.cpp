#define GRAPHICS_ENGINE_LOG_COMPILED_MINIMUM_SEVERITY 5
#include <GraphicsEngine/lib/logging/logger.h>
#include <benchmark/benchmark.h>

namespace {
void BM_LoggingRuntimeDisabledCategory(benchmark::State& state) {
    NLogging::Logger logger({.DisabledCategories = {"disabled"}});
    logger.Start();
    for ([[maybe_unused]] auto _: state) {
        benchmark::DoNotOptimize(logger.Log(NLogging::ESeverity::INFO, "disabled", "ignored"));
    }
}

void BM_LoggingCompileTimeDisabledSeverity(benchmark::State& state) {
    NLogging::Logger logger;
    int messageEvaluations = 0;
    for ([[maybe_unused]] auto _: state) {
        GRAPHICS_ENGINE_LOG(logger, NLogging::ESeverity::INFO, "compile", (++messageEvaluations, "compiled out"));
    }
    benchmark::DoNotOptimize(messageEvaluations);
}
} // namespace

BENCHMARK(BM_LoggingRuntimeDisabledCategory);
BENCHMARK(BM_LoggingCompileTimeDisabledSeverity);
