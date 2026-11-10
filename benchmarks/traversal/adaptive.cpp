#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include <GraphicsEngine/ecs/world.h>
#include <benchmark/benchmark.h>
#include <ecs/internal/adaptive_traversal.h>

namespace {
void BM_AdaptiveTraversal(benchmark::State& state) {
    const auto shape = state.range(0);                             // balanced, star, bamboo, random, skewed
    const auto workers = static_cast<std::size_t>(state.range(1)); // 0 = iterative serial reference
    const auto iterations = static_cast<std::size_t>(state.range(2));
    NEcs::World world;
    std::vector<NEcs::EntityReference> entities;
    // NOLINTNEXTLINE(cert-msc51-cpp): Fixed seed makes the topology reproducible.
    std::mt19937 random{93};
    for (std::size_t index = 0; index < 4096; ++index) {
        entities.push_back(world.GetEntityReference(world.CreateEntity()));
        if (index == 0) {
            continue;
        }
        std::size_t parent = 0;
        switch (shape) {
        case 0:
            parent = (index - 1) / 4;
            break;
        case 1:
            break;
        case 2:
            parent = index - 1;
            break;
        case 3:
            parent = random() % index;
            break;
        default:
            parent = index % 5 == 0 ? 0 : index - 1;
            break;
        }
        world.DeferReparent(entities[index], entities[parent]);
    }
    world.ApplyDeferredStructuralChanges();
    const std::vector roots{entities.front()};
    NCommon::TaskSystem tasks{std::max(workers, std::size_t{1})};
    NEcs::NInternal::AdaptiveBatchPlanner planner{tasks};
    for ([[maybe_unused]] auto _: state) {
        NEcs::NInternal::TraversalOperation<std::uint64_t> operation{
                .Visit =
                        [iterations](NEcs::NInternal::TraversalInput<std::uint64_t> input) {
                            std::uint64_t value =
                                    input.Entity.Identity.Index + (input.ParentResult ? *input.ParentResult : 1);
                            const auto cost = input.Entity.Identity.Index % 17 == 0 ? iterations * 4 : iterations;
                            for (std::size_t step = 0; step < cost; ++step) {
                                value ^= value >> 12;
                                value ^= value << 25;
                                value ^= value >> 27;
                                value *= 2685821657736338717ULL;
                            }
                            benchmark::DoNotOptimize(value);
                            return value;
                        },
                .Output = std::make_shared<NEcs::NInternal::TraversalOutput<std::uint64_t>>(),
                .EstimateWork =
                        [](NEcs::EntityReference entity) { return entity.Identity.Index % 17 == 0 ? 4.0 : 1.0; },
        };
        const auto output = operation.Output;
        const auto started = std::chrono::steady_clock::now();
        if (workers == 0) {
            const auto result =
                    NEcs::NInternal::AdaptiveBatchPlanner::RunSerial(world.AcquireTopologySnapshot(),
                                                                     roots,
                                                                     NEcs::NInternal::ETraversalMode::TOP_DOWN,
                                                                     std::move(operation));
            auto value = result.Get(entities.back());
            benchmark::DoNotOptimize(value);
        } else {
            const auto group = planner.ScheduleTraversal(world.AcquireTopologySnapshot(),
                                                         roots,
                                                         NEcs::NInternal::ETraversalMode::TOP_DOWN,
                                                         std::move(operation));
            tasks.WaitGroup(group);
            const auto result = output->TryAcquire(tasks);
            if (!result) {
                state.SkipWithError("traversal failed");
                break;
            }
            auto value = result->Get(entities.back());
            benchmark::DoNotOptimize(value);
            const auto metrics = output->GetMetrics();
            state.counters["packets"] = static_cast<double>(metrics.Packets);
            state.counters["peak_packets"] = static_cast<double>(metrics.PeakActivePackets);
            state.counters["schedule_ns"] = static_cast<double>(metrics.SchedulingNanoseconds);
            state.counters["target_units"] = metrics.TargetWorkUnits;
        }
        state.SetIterationTime(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    }
}
BENCHMARK(BM_AdaptiveTraversal)->ArgsProduct({{0, 1, 2, 3, 4}, {0, 1, 4}, {0, 10000}})->UseManualTime();
} // namespace
