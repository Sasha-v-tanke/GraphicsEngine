#include <cstdint>
#include <memory>
#include <utility>

#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/scene/scene.h>
#include <benchmark/benchmark.h>
#include <lib/thread/task/task_system.h>

namespace {
struct QueryValue {
    std::uint64_t Number = 1;
};

// The legacy callback remains the baseline: entity snapshot plus hash lookups.
void Visit(const NEcs::World& world, int mode, std::uint64_t& sum) {
    switch (mode) {
    case 0:
        world.Query<QueryValue>([&](NEcs::Entity, const QueryValue& value) { sum += value.Number; });
        break;
    case 1:
        world.ForEach<QueryValue>([&](const NEcs::EntityRef&, const QueryValue& value) { sum += value.Number; });
        break;
    case 2:
        for (auto [entity, value]: world.Query<QueryValue>()) {
            benchmark::DoNotOptimize(entity);
            sum += value.Number;
        }
        break;
    }
}

void BM_Query(benchmark::State& state) {
    NEcs::World world;
    const auto count = static_cast<std::uint32_t>(state.range(0));
    const int mode = static_cast<int>(state.range(1));
    for (std::uint32_t index = 0; index < count; ++index) {
        world.AddComponent<QueryValue>(world.CreateEntity(), index + 1);
    }
    const auto expected = std::uint64_t{count} * (count + 1) / 2;
    for ([[maybe_unused]] auto _: state) {
        std::uint64_t sum = 0;
        Visit(world, mode, sum);
        benchmark::DoNotOptimize(sum);
        if (sum != expected) {
            state.SkipWithError("Query membership mismatch");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_Query)->ArgsProduct({{64, 4096, 65536}, {0, 1, 2}});

void BM_SystemQuery(benchmark::State& state) {
    NEcs::World world;
    for (std::uint32_t index = 0; index < 4096; ++index) {
        world.AddComponent<QueryValue>(world.CreateEntity(), index + 1);
    }
    const int mode = static_cast<int>(state.range(0));
    std::uint64_t sum = 0;
    world.RegisterSystem(
            {NEcs::Read<QueryValue>()},
            [&](NEcs::World& current) { Visit(current, mode, sum); },
            NEcs::SystemSchedule{.Id = 1});
    NCommon::TaskSystem tasks{static_cast<std::size_t>(state.range(1))};
    for ([[maybe_unused]] auto _: state) {
        sum = 0;
        world.RunSystems(tasks);
        benchmark::DoNotOptimize(sum);
        if (sum != std::uint64_t{4096} * 4097 / 2) {
            state.SkipWithError("System Query mismatch");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * 4096);
}
BENCHMARK(BM_SystemQuery)->ArgsProduct({{0, 1, 2}, {1, 2, 8}})->UseRealTime();
class TypedQuerySystem {
public:
    using Access = NEcs::NSystem::Access<NEcs::NSystem::Read<QueryValue>>;
    using Context = NEcs::NSystem::SystemContext<Access>;

    explicit TypedQuerySystem(std::uint64_t& sum)
        : m_sum(sum) {
    }

    void Update(Context& context) {
        for (auto [entity, value]: context.Query<QueryValue>()) {
            benchmark::DoNotOptimize(entity);
            m_sum += value.Number;
        }
    }

private:
    std::uint64_t& m_sum;
};

void BM_TypedSystemQuery(benchmark::State& state) {
    NScene::Scene scene;
    auto& world = scene.GetWorld();
    const auto count = static_cast<std::uint32_t>(state.range(0));
    for (std::uint32_t index = 0; index < count; ++index) {
        world.AddComponent<QueryValue>(world.CreateEntity(), index + 1);
    }
    const auto expected = std::uint64_t{count} * (count + 1) / 2;
    std::uint64_t sum = 0;
    scene.AddSystem<TypedQuerySystem>(sum);
    NCommon::TaskSystem tasks{static_cast<std::size_t>(state.range(1))};
    for ([[maybe_unused]] auto _: state) {
        sum = 0;
        world.RunSystems(tasks);
        benchmark::DoNotOptimize(sum);
        if (sum != expected) {
            state.SkipWithError("Typed System Query mismatch");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_TypedSystemQuery)->ArgsProduct({{1024, 4096, 16384, 65536}, {1, 2, 8}})->UseRealTime();
} // namespace
