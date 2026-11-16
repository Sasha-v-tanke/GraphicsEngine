#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <benchmark/benchmark.h>
#include <ecs/internal/adaptive_traversal.h>
#include <ecs/internal/transform_resolver.h>
#include <tests/ecs_concurrency/forest.h>

namespace {
using Clock = std::chrono::steady_clock;

void BM_EcsAcceptanceTraversal(benchmark::State& state) {
    const auto shape = static_cast<NEcsAcceptance::EShape>(state.range(0));
    const auto count = static_cast<std::size_t>(state.range(1));
    const auto workers = static_cast<std::size_t>(state.range(2));
    const auto cost = static_cast<std::size_t>(state.range(3));
    NEcs::World world;
    NEcsAcceptance::Forest forest{world, count, shape};
    NCommon::TaskSystem tasks{std::max(workers, std::size_t{1})};
    NEcs::NInternal::AdaptiveBatchPlanner planner{tasks};
    const std::array roots{forest.Entities.front()};
    // Check against a separately implemented evaluator before timing a workload.
    const auto reference = forest.TraverseReference(false, false, cost);
    for ([[maybe_unused]] auto iteration: state) {
        NEcs::NInternal::TraversalOperation<std::uint64_t> operation;
        operation.Output = std::make_shared<NEcs::NInternal::TraversalOutput<std::uint64_t>>();
        operation.Visit = [cost](auto input) {
            return NEcsAcceptance::Work((input.ParentResult ? *input.ParentResult : 17) * 31 +
                                                input.Entity.Identity.Index,
                                        cost);
        };
        const auto output = operation.Output;
        const auto started = Clock::now();
        std::uint64_t last = 0;
        if (workers == 0) {
            const auto actual =
                    NEcs::NInternal::AdaptiveBatchPlanner::RunSerial(world.AcquireTopologySnapshot(),
                                                                     roots,
                                                                     NEcs::NInternal::ETraversalMode::TOP_DOWN,
                                                                     std::move(operation));
            last = actual.Get(forest.Entities.back());
        } else {
            const auto group = planner.ScheduleTraversal(world.AcquireTopologySnapshot(),
                                                         roots,
                                                         NEcs::NInternal::ETraversalMode::TOP_DOWN,
                                                         std::move(operation));
            tasks.WaitGroup(group);
            const auto actual = output->TryAcquire(tasks);
            if (!actual) {
                state.SkipWithError("Traversal did not publish a complete result");
                break;
            }
            last = actual->Get(forest.Entities.back());
        }
        state.SetIterationTime(std::chrono::duration<double>(Clock::now() - started).count());
        benchmark::DoNotOptimize(last);
        if (last != reference.back()) {
            state.SkipWithError("Independent serial reference mismatch");
            break;
        }
        if (workers != 0) {
            const auto metrics = output->GetMetrics();
            state.counters["packets"] = static_cast<double>(metrics.Packets);
            state.counters["peak_packets"] = static_cast<double>(metrics.PeakActivePackets);
            state.counters["schedule_ns"] = static_cast<double>(metrics.SchedulingNanoseconds);
            state.counters["work_ns"] = static_cast<double>(metrics.WorkNanoseconds);
            state.counters["target_units"] = metrics.TargetWorkUnits;
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}
BENCHMARK(BM_EcsAcceptanceTraversal)
        ->ArgsProduct({{0, 1, 2, 3, 4}, {1024, 16384, 1000000}, {0, 1, 2, 4, 8, 16}, {0, 1024}})
        ->UseManualTime();

void BM_EcsAcceptanceTransforms(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    const auto workers = static_cast<std::size_t>(state.range(1));
    const auto profile = state.range(2); // cold, hot, sparse local edit, dirty root
    NEcs::World world;
    NEcsAcceptance::Forest forest{world, count, NEcsAcceptance::EShape::BALANCED};
    NCommon::TaskSystem tasks{workers};
    auto resolver = std::make_unique<NEcs::NInternal::TransformResolver>();
    const auto initial = resolver->ScheduleResolve(world.AcquireTopologySnapshot(), forest.Entities, tasks);
    tasks.Wait(initial.GetCompletion());
    std::size_t serial = 0;
    for ([[maybe_unused]] auto iteration: state) {
        if (profile == 0) {
            resolver = std::make_unique<NEcs::NInternal::TransformResolver>();
        } else if (profile > 1) {
            auto local = NEcsAcceptance::Identity();
            local[3] = static_cast<float>(++serial % 1024);
            forest.SetLocal(profile == 2 ? count - 1 : 0, local);
        }
        const auto started = Clock::now();
        const auto operation = resolver->ScheduleResolve(world.AcquireTopologySnapshot(), forest.Entities, tasks);
        tasks.Wait(operation.GetCompletion());
        if (tasks.GetStatus(operation.GetCompletion()) != NCommon::ETaskStatus::COMPLETED) {
            state.SkipWithError("Resolve did not publish a complete result");
            break;
        }
        const auto result = operation.ReadResolved(tasks);
        state.SetIterationTime(std::chrono::duration<double>(Clock::now() - started).count());
        auto value = result.Get(forest.Entities.back()).Value;
        benchmark::DoNotOptimize(value);
        state.counters["resolved"] = static_cast<double>(result.GetResolvedCount());
        state.counters["recomputed"] = static_cast<double>(result.GetRecomputedCount());
        state.counters["cache_hits"] = static_cast<double>(result.GetResolvedCount() - result.GetRecomputedCount());
    }
}
BENCHMARK(BM_EcsAcceptanceTransforms)->ArgsProduct({{1024, 16384}, {1, 2, 4, 8, 16}, {0, 1, 2, 3}})->UseManualTime();
} // namespace
