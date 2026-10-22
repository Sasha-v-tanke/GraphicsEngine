#include <memory>
#include <memory_resource>

#include <benchmark/benchmark.h>
#include <ecs/world.h>
#include <engine/controller/frame_scheduler.h>
#include <graphics/material.h>
#include <renderer/extraction.h>
#include <renderer/render_components.h>
#include <renderer/renderer.h>
#include <resources/resource_manager.h>

namespace NResources {
class MeshData {};
} // namespace NResources

namespace {

void BM_RendererPreparation(benchmark::State& state) {
    NResources::ResourceManager resources;
    const auto mesh = resources.Request<NResources::MeshData>({"mesh", "benchmark"});
    resources.PublishReady(resources.BeginLoading(mesh), std::make_shared<NResources::MeshData>());
    const auto material = resources.Request<NGraphics::Material>({"material", "benchmark"});
    resources.PublishReady(
            resources.BeginLoading(material),
            std::make_shared<NGraphics::Material>(NResources::ResourceIdentity{"pipeline", "benchmark"}));
    NEcs::World world;
    for (int index = 0; index < 2; ++index) {
        const auto entity = world.CreateEntity();
        world.AddComponent<NRenderer::TransformComponent>(entity);
        world.AddComponent<NRenderer::CameraComponent>(entity);
    }
    for (int index = 0; index < state.range(0); ++index) {
        const auto entity = world.CreateEntity();
        world.AddComponent<NRenderer::TransformComponent>(entity);
        world.AddComponent<NRenderer::RenderableComponent>(
                entity,
                NRenderer::RenderableComponent{.Mesh = mesh, .Material = material});
    }
    NEngine::NController::FrameScheduler scheduler{NEngine::EngineConfig{.MaxActiveFrames = 1}};
    const auto frame = *scheduler.TryAcquireFrame();
    scheduler.ArmFrame(frame);
    scheduler.BeginUpdate(frame);
    scheduler.EndUpdate(frame);
    scheduler.SignalDraw(frame);
    scheduler.BeginFinalize(frame);
    const auto& snapshot = NRenderer::NInternal::ExtractRenderWorld(world, resources, scheduler.GetFrameStorage(frame));
    NCommon::TaskSystem tasks{4};
    for ([[maybe_unused]] auto _: state) {
        std::pmr::monotonic_buffer_resource memory;
        const NRenderer::RenderPlan* plan = nullptr;
        if (state.range(1) == 0) {
            plan = &NRenderer::Renderer::Prepare(snapshot, memory);
        } else {
            NCommon::TaskHandle join;
            const auto root = tasks.Submit([&](NCommon::TaskContext& task) {
                join = NRenderer::Renderer::SchedulePrepare(
                        snapshot,
                        memory,
                        task,
                        [&](const NRenderer::RenderPlan& prepared) { plan = &prepared; },
                        static_cast<std::size_t>(state.range(1)));
            });
            tasks.Wait(root);
            if (!join.IsValid()) {
                state.SkipWithError("Preparation scheduling failed");
                break;
            }
            tasks.Wait(join);
        }
        if (plan == nullptr) {
            state.SkipWithError("Preparation failed");
            break;
        }
        benchmark::DoNotOptimize(plan->GetDrawCommands().data());
        benchmark::DoNotOptimize(plan->GetBatches().data());
    }
}

BENCHMARK(BM_RendererPreparation)->ArgsProduct({{1024, 16384}, {0, 64, 256, 1024, 4096}})->UseRealTime();

} // namespace
