#include <memory>

#include <benchmark/benchmark.h>
#include <ecs/internal/scoped_batch.h>
#include <ecs/world.h>
#include <engine/controller/frame_scheduler.h>
#include <graphics/material.h>
#include <renderer/extraction.h>
#include <resources/mesh_data.h>

namespace {

void BM_RenderWorldExtraction(benchmark::State& state) {
    NResources::ResourceManager resources;
    const auto mesh = resources.Request<NResources::MeshData>({"mesh", "extraction"});
    resources.PublishReady(resources.BeginLoading(mesh), std::make_shared<NResources::MeshData>());
    const auto material = resources.Request<NGraphics::Material>({"material", "extraction"});
    resources.PublishReady(
            resources.BeginLoading(material),
            std::make_shared<NGraphics::Material>(NResources::ResourceIdentity{"pipeline", "extraction"}));
    NEcs::World world;
    const auto camera = world.CreateEntity();
    world.AddComponent<NRenderer::TransformComponent>(camera);
    world.AddComponent<NRenderer::CameraComponent>(camera);
    for (std::int64_t index = 0; index < state.range(0); ++index) {
        const auto entity = world.CreateEntity();
        world.AddComponent<NRenderer::TransformComponent>(entity);
        world.AddComponent<NRenderer::RenderableComponent>(
                entity,
                NRenderer::RenderableComponent{.Mesh = mesh, .Material = material});
    }
    NCommon::TaskSystem tasks{static_cast<std::size_t>(state.range(2))};
    NRenderer::NInternal::IncrementalExtraction cache;
    NEngine::NController::FrameScheduler frames{NEngine::EngineConfig{.MaxActiveFrames = 1}};
    if (state.range(3) != 0) {
        NEcs::Entity target;
        std::as_const(world).Query<NRenderer::RenderableComponent>(
                [&](NEcs::Entity entity, const auto&) { target = entity; });
        const auto reference = world.GetEntityReference(target);
        NEcs::NInternal::ScopedWorldAccess::Register(
                world,
                {NEcs::NInternal::ScopedWrite<NRenderer::TransformComponent>(
                        NEcs::NInternal::BatchScope::Entity(reference))},
                [reference](NEcs::NInternal::ScopedBatchContext& context) {
                    context.Write<NRenderer::TransformComponent>().Get(reference).Value.Translation.X += 1;
                },
                NEcs::SystemSchedule{.Id = 1});
    }
    bool warm = false;
    for ([[maybe_unused]] auto _: state) {
        state.PauseTiming();
        const auto frame = *frames.TryAcquireFrame();
        frames.ArmFrame(frame);
        frames.BeginUpdate(frame);
        frames.EndUpdate(frame);
        frames.SignalDraw(frame);
        frames.BeginFinalize(frame);
        const auto storage = frames.GetFrameStorage(frame);
        if (state.range(3) != 0) {
            world.RunSystems(tasks);
        }
        if (!warm && state.range(1) != 0) {
            NCommon::TaskHandle initial;
            const auto root = tasks.Submit([&](NCommon::TaskContext& task) {
                initial = cache.Schedule(world, resources, storage, tasks, task, [](const auto&) {});
            });
            tasks.Wait(root);
            tasks.Wait(initial);
            warm = true;
        }
        state.ResumeTiming();
        const NRenderer::RenderWorld* snapshot = nullptr;
        NCommon::TaskHandle join;
        const auto root = tasks.Submit([&](NCommon::TaskContext& task) {
            const auto complete = [&](const auto& result) { snapshot = &result; };
            join = state.range(1) == 0 ? NRenderer::NInternal::ScheduleRenderWorldExtraction(world,
                                                                                             resources,
                                                                                             storage,
                                                                                             task,
                                                                                             complete)
                                       : cache.Schedule(world, resources, storage, tasks, task, complete);
        });
        tasks.Wait(root);
        if (join.IsValid()) {
            tasks.Wait(join);
        }
        if (!snapshot) {
            state.SkipWithError("Extraction did not publish");
            break;
        }
        benchmark::DoNotOptimize(snapshot->GetObjects().data());
        state.PauseTiming();
        frames.CompleteFrame(frame);
        frames.RecycleFrame(frame);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_RenderWorldExtraction)->ArgsProduct({{1024, 16384}, {0, 1}, {1, 4}, {0, 1}})->UseRealTime();

} // namespace
