#include <array>
#include <chrono>
#include <cstddef>
#include <vector>

#include <GraphicsEngine/ecs/world.h>
#include <benchmark/benchmark.h>
#include <ecs/internal/transform_resolver.h>

namespace {
void BM_TransformResolve(benchmark::State& state) {
    const auto workers = static_cast<std::size_t>(state.range(0));
    const bool allRequired = state.range(1) != 0;
    const bool dirty = state.range(2) != 0;
    NEcs::World world;
    std::vector<NEcs::EntityReference> entities;
    for (std::size_t index = 0; index < 4096; ++index) {
        entities.push_back(world.GetEntityReference(world.CreateEntity()));
        if (index != 0) {
            world.DeferReparent(entities.back(), entities[(index - 1) / 4]);
        }
    }
    world.ApplyDeferredStructuralChanges();
    NCommon::TaskSystem tasks{workers};
    NEcs::NInternal::TransformResolver resolver;
    const std::array last{entities.back()};
    const auto required = allRequired ? std::span<const NEcs::EntityReference>{entities}
                                      : std::span<const NEcs::EntityReference>{last};
    const auto initial = resolver.ScheduleResolve(world.AcquireTopologySnapshot(), required, tasks);
    tasks.Wait(initial.GetCompletion());
    auto local = NMath::Mat4::Identity();
    for ([[maybe_unused]] auto iteration: state) {
        if (dirty) {
            local(0, 3) += 1;
            world.SetLocalTransform(entities.front(), local);
        }
        const auto started = std::chrono::steady_clock::now();
        const auto operation = resolver.ScheduleResolve(world.AcquireTopologySnapshot(), required, tasks);
        tasks.Wait(operation.GetCompletion());
        if (tasks.GetStatus(operation.GetCompletion()) != NCommon::ETaskStatus::COMPLETED) {
            state.SkipWithError("transform resolve failed");
            break;
        }
        const auto resolved = operation.ReadResolved(tasks);
        benchmark::DoNotOptimize(resolved.Get(entities.back()).Value);
        state.SetIterationTime(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        state.counters["resolved"] = static_cast<double>(resolved.GetResolvedCount());
        state.counters["recomputed"] = static_cast<double>(resolved.GetRecomputedCount());
    }
}
BENCHMARK(BM_TransformResolve)->ArgsProduct({{1, 4}, {0, 1}, {0, 1}})->UseManualTime();
} // namespace
