#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <engine/controller/frame_scheduler.h>
#include <graphics/material.h>
#include <gtest/gtest.h>
#include <renderer/extraction.h>
#include <renderer/render_components.h>
#include <renderer/renderer.h>
#include <resources/async_resource_loader.h>
#include <resources/mesh_loader.h>

namespace {

using NEngine::NController::FrameHandle;
using NRenderer::ERenderResourceRole;
using NRenderer::RenderWorld;
using NResources::EResourceState;
using NResources::MeshData;

template<typename Predicate>
bool Until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

struct Gate {
    std::mutex Mutex;
    std::condition_variable_any Wake;
    std::atomic_bool Entered = false;
    bool Released = false;

    void Wait(const std::stop_token& stop) {
        Entered = true;
        std::unique_lock lock{Mutex};
        Wake.wait(lock, stop, [&] { return Released; });
    }
    void Open() {
        std::lock_guard lock{Mutex};
        Released = true;
        Wake.notify_all();
    }
};

class ResourceReadiness: public testing::TestWithParam<std::size_t> {
protected:
    NResources::ResourceManager Resources;
    NEcs::World World;
    NEngine::NController::FrameScheduler Frames{NEngine::EngineConfig{.MaxActiveFrames = 3}};
    NResources::ResourceHandle<NGraphics::Material> Material;
    NEcs::Entity Object;

    void SetUp() override {
        Material = Resources.Request<NGraphics::Material>({"material", "ready"});
        Resources.PublishReady(Resources.BeginLoading(Material),
                               std::make_shared<NGraphics::Material>(NResources::ResourceIdentity{"pipeline", "test"}));
        const auto camera = World.CreateEntity();
        World.AddComponent<NRenderer::TransformComponent>(camera);
        World.AddComponent<NRenderer::CameraComponent>(camera);
        Object = World.CreateEntity();
        World.AddComponent<NRenderer::TransformComponent>(Object);
        World.AddComponent<NRenderer::RenderableComponent>(
                Object,
                NRenderer::RenderableComponent{.Mesh = {}, .Material = Material});
    }

    FrameHandle Begin() {
        const auto frame = Frames.TryAcquireFrame();
        EXPECT_TRUE(frame);
        if (!frame) {
            throw std::runtime_error("No test frame slot available");
        }
        Frames.ArmFrame(*frame);
        Frames.BeginUpdate(*frame);
        Frames.EndUpdate(*frame);
        Frames.SignalDraw(*frame);
        Frames.BeginFinalize(*frame);
        return *frame;
    }

    const RenderWorld* Extract(FrameHandle frame, NCommon::TaskSystem& tasks, std::size_t chunkSize = 1) {
        const RenderWorld* result = nullptr;
        NCommon::TaskHandle join;
        const auto root = tasks.Submit([&](NCommon::TaskContext& task) {
            join = NRenderer::NInternal::ScheduleRenderWorldExtraction(
                    World,
                    Resources,
                    Frames.GetFrameStorage(frame),
                    task,
                    [&](const RenderWorld& snapshot) { result = &snapshot; },
                    chunkSize);
        });
        tasks.Wait(root);
        EXPECT_EQ(tasks.GetStatus(root), NCommon::ETaskStatus::COMPLETED);
        if (!join.IsValid()) {
            return nullptr;
        }
        tasks.Wait(join);
        EXPECT_EQ(tasks.GetStatus(join), NCommon::ETaskStatus::COMPLETED);
        return result;
    }

    void Release(FrameHandle frame) {
        Frames.CompleteFrame(frame);
        Frames.RecycleFrame(frame);
    }

    std::size_t CommandCount(const RenderWorld& snapshot, FrameHandle frame) {
        return NRenderer::Renderer::Prepare(snapshot, Frames.GetFrameStorage(frame).GetMemoryResource())
                .GetDrawCommands()
                .size();
    }

    static NResources::ResourceIoBytes Bytes() {
        const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        const auto bytes = std::as_bytes(std::span{obj});
        return {bytes.begin(), bytes.end()};
    }
};

TEST_P(ResourceReadiness, LoadingUploadAndReadyAffectOnlyFutureActiveFrames) {
    NCommon::TaskSystem tasks{GetParam()};
    Gate read;
    std::atomic_bool complete = false;
    auto gpu = std::make_shared<int>(1);
    std::weak_ptr<int> gpuLifetime = gpu;
    NResources::AsyncResourceLoader loader{Resources, tasks, {}, [&](const auto&, const auto& stop, auto) {
                                               read.Wait(stop);
                                               return Bytes();
                                           }};
    const auto mesh = loader.LoadMesh({"mesh", "streamed.obj"}, "streamed.obj", [&](const auto&) {
        return NResources::ResourceUpload{gpu, [&] { return complete.load(); }, [&] { complete = true; }};
    });
    World.GetComponent<NRenderer::RenderableComponent>(Object).Mesh = mesh;
    ASSERT_TRUE(Until([&] { return read.Entered.load(); }));
    const auto first = Begin();
    const auto* loading = Extract(first, tasks);
    ASSERT_NE(loading, nullptr);
    EXPECT_TRUE(loading->GetObjects().empty());
    ASSERT_EQ(loading->GetResourceDiagnostics().size(), 1U);
    EXPECT_EQ(loading->GetResourceDiagnostics()[0].Source.State, EResourceState::QUEUED);
    EXPECT_EQ(loading->GetResourceDiagnostics()[0].Source.Identity->GetKey(), "streamed.obj");
    std::promise<void> progress;
    const auto progressTask = tasks.Submit([&](NCommon::TaskContext&) { progress.set_value(); });
    EXPECT_EQ(progress.get_future().wait_for(std::chrono::seconds{2}), std::future_status::ready);
    tasks.Wait(progressTask);
    read.Open();
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(mesh) == EResourceState::UPLOADING;
    }));
    const auto second = Begin();
    const auto* uploading = Extract(second, tasks);
    ASSERT_NE(uploading, nullptr);
    ASSERT_EQ(uploading->GetResourceDiagnostics().size(), 1U);
    EXPECT_EQ(uploading->GetResourceDiagnostics()[0].Source.State, EResourceState::UPLOADING);
    EXPECT_TRUE(uploading->GetObjects().empty());
    complete = true;
    loader.PumpUploads();
    const auto third = Begin();
    const auto* ready = Extract(third, tasks);
    ASSERT_NE(ready, nullptr);
    ASSERT_EQ(ready->GetObjects().size(), 1U);
    EXPECT_TRUE(ready->GetResourceDiagnostics().empty());
    EXPECT_EQ(CommandCount(*loading, first), 0U);
    EXPECT_EQ(CommandCount(*uploading, second), 0U);
    EXPECT_EQ(CommandCount(*ready, third), 1U);
    EXPECT_EQ(loading->GetResourceDiagnostics()[0].Source.State, EResourceState::QUEUED);
    EXPECT_EQ(uploading->GetResourceDiagnostics()[0].Source.State, EResourceState::UPLOADING);
    auto submittedUse = ready->GetResourceUseRecords()[0];
    Resources.RequestUnload(mesh);
    Resources.CompleteUnload(mesh);
    Resources.Forget(mesh);
    loader.Shutdown();
    gpu.reset();
    Release(first);
    Release(second);
    EXPECT_FALSE(gpuLifetime.expired());
    Release(third);
    EXPECT_FALSE(gpuLifetime.expired());
    submittedUse = {};
    EXPECT_TRUE(gpuLifetime.expired());
}

TEST_P(ResourceReadiness, FailedRetryAndUnloadPreserveSnapshotDiagnosticsAndLeases) {
    NCommon::TaskSystem tasks{GetParam()};
    std::atomic_bool fail = true;
    NResources::AsyncResourceLoader loader{Resources, tasks, {}, [](const auto&, const auto&, auto) {
                                               return Bytes();
                                           }};
    const auto decoder = [&](auto bytes, const auto&) -> std::shared_ptr<const MeshData> {
        if (fail) {
            throw std::runtime_error("malformed mesh data");
        }
        return NResources::DecodeMesh(bytes);
    };
    const auto mesh = loader.Load<MeshData>({"mesh", "retry.obj"}, "/assets/retry.obj", decoder);
    World.GetComponent<NRenderer::RenderableComponent>(Object).Mesh = mesh;
    ASSERT_TRUE(Until([&] { return Resources.GetState(mesh) == EResourceState::FAILED; }));
    const auto first = Begin();
    const auto* failed = Extract(first, tasks);
    ASSERT_NE(failed, nullptr);
    ASSERT_EQ(failed->GetResourceDiagnostics().size(), 1U);
    const auto& diagnostic = failed->GetResourceDiagnostics()[0];
    EXPECT_EQ(diagnostic.Object.Index, Object.Index);
    EXPECT_EQ(diagnostic.Role, ERenderResourceRole::MESH);
    EXPECT_EQ(diagnostic.Policy, NRenderer::EResourceReadinessPolicy::SKIP_OBJECT);
    EXPECT_EQ(diagnostic.Source.State, EResourceState::FAILED);
    ASSERT_TRUE(diagnostic.Source.Failure);
    const std::string originalError = diagnostic.Source.Failure->Message;
    EXPECT_NE(originalError.find("/assets/retry.obj"), std::string::npos);
    EXPECT_NE(originalError.find("malformed mesh data"), std::string::npos);
    fail = false;
    EXPECT_EQ(loader.Load<MeshData>({"mesh", "retry.obj"}, "/assets/retry.obj", decoder), mesh);
    ASSERT_TRUE(Until([&] {
        loader.PumpUploads();
        return Resources.GetState(mesh) == EResourceState::READY;
    }));
    const auto second = Begin();
    const auto* ready = Extract(second, tasks);
    ASSERT_NE(ready, nullptr);
    ASSERT_EQ(ready->GetObjects().size(), 1U);
    Resources.RequestUnload(mesh);
    Resources.CompleteUnload(mesh);
    Resources.Forget(mesh);
    EXPECT_EQ(failed->GetResourceDiagnostics()[0].Source.Failure->Message, originalError);
    EXPECT_EQ(failed->GetResourceDiagnostics()[0].Source.Identity->GetKey(), "retry.obj");
    EXPECT_EQ(CommandCount(*failed, first), 0U);
    EXPECT_EQ(CommandCount(*ready, second), 1U);
    EXPECT_EQ(ready->GetObjects()[0].Mesh->GetVertices().size(), 3U);
}

TEST_P(ResourceReadiness, ParallelDecisionIsLatchedBeforeChunkExecution) {
    NCommon::TaskSystem tasks{GetParam()};
    const auto mesh = Resources.Request<MeshData>({"mesh", "late-ready"});
    const auto operation = Resources.BeginLoading(mesh);
    World.GetComponent<NRenderer::RenderableComponent>(Object).Mesh = mesh;
    const auto first = Begin();
    std::promise<void> captured;
    std::promise<void> resume;
    const auto release = resume.get_future().share();
    NCommon::TaskHandle join;
    const RenderWorld* snapshot = nullptr;
    const auto root = tasks.Submit([&](NCommon::TaskContext& task) {
        join = NRenderer::NInternal::ScheduleRenderWorldExtraction(
                World,
                Resources,
                Frames.GetFrameStorage(first),
                task,
                [&](const RenderWorld& result) { snapshot = &result; },
                1);
        captured.set_value();
        release.wait();
    });
    captured.get_future().wait();
    Resources.PublishReady(operation, NResources::DecodeMesh(Bytes()));
    resume.set_value();
    tasks.Wait(root);
    tasks.Wait(join);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(snapshot->GetObjects().empty());
    ASSERT_EQ(snapshot->GetResourceDiagnostics().size(), 1U);
    EXPECT_EQ(snapshot->GetResourceDiagnostics()[0].Source.State, EResourceState::LOADING);
    const auto second = Begin();
    const auto* future = Extract(second, tasks);
    ASSERT_NE(future, nullptr);
    EXPECT_EQ(future->GetObjects().size(), 1U);
    EXPECT_TRUE(future->GetResourceDiagnostics().empty());
}

TEST_P(ResourceReadiness, ParallelLeasesSurviveUnloadAndSlotReuseBeforeChunks) {
    NCommon::TaskSystem tasks{GetParam()};
    const auto mesh = Resources.Request<MeshData>({"mesh", "old"});
    Resources.PublishReady(Resources.BeginLoading(mesh), NResources::DecodeMesh(Bytes()));
    World.GetComponent<NRenderer::RenderableComponent>(Object).Mesh = mesh;
    const auto first = Begin();
    std::promise<void> captured;
    std::promise<void> resume;
    const auto release = resume.get_future().share();
    NCommon::TaskHandle join;
    const RenderWorld* snapshot = nullptr;
    const auto root = tasks.Submit([&](NCommon::TaskContext& task) {
        join = NRenderer::NInternal::ScheduleRenderWorldExtraction(
                World,
                Resources,
                Frames.GetFrameStorage(first),
                task,
                [&](const RenderWorld& result) { snapshot = &result; },
                1);
        captured.set_value();
        release.wait();
    });
    captured.get_future().wait();
    Resources.RequestUnload(mesh);
    Resources.CompleteUnload(mesh);
    Resources.Forget(mesh);
    const auto replacement = Resources.Request<MeshData>({"mesh", "replacement"});
    Resources.PublishReady(Resources.BeginLoading(replacement), NResources::DecodeMesh(Bytes()));
    resume.set_value();
    tasks.Wait(root);
    tasks.Wait(join);
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->GetObjects().size(), 1U);
    EXPECT_EQ(snapshot->GetObjects()[0].Mesh.GetResource(), mesh);
    EXPECT_NE(snapshot->GetObjects()[0].Mesh.GetResource(), replacement);
    EXPECT_EQ(CommandCount(*snapshot, first), 1U);
}

TEST_P(ResourceReadiness, SerialAndParallelKeepDeterministicSkipDiagnostics) {
    NCommon::TaskSystem tasks{GetParam()};
    const auto mesh = Resources.Request<MeshData>({"mesh", "failed"});
    Resources.Fail(Resources.BeginLoading(mesh), {.Code = NCommon::EError::IO_ERROR, .Message = "failed.obj: decode"});
    for (int index = 0; index < 101; ++index) {
        const auto entity = World.CreateEntity();
        World.AddComponent<NRenderer::TransformComponent>(entity);
        World.AddComponent<NRenderer::RenderableComponent>(
                entity,
                NRenderer::RenderableComponent{.Mesh = mesh, .Material = {}});
    }
    World.GetComponent<NRenderer::RenderableComponent>(Object).Mesh = mesh;
    const auto first = Begin();
    const auto& serial = NRenderer::NInternal::ExtractRenderWorld(World, Resources, Frames.GetFrameStorage(first));
    const auto second = Begin();
    const auto* parallel = Extract(second, tasks, 7);
    ASSERT_NE(parallel, nullptr);
    EXPECT_TRUE(serial.GetObjects().empty());
    EXPECT_TRUE(parallel->GetObjects().empty());
    const auto expected = serial.GetResourceDiagnostics();
    const auto actual = parallel->GetResourceDiagnostics();
    ASSERT_EQ(expected.size(), 203U);
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(actual[index].Object, expected[index].Object);
        EXPECT_EQ(actual[index].Role, expected[index].Role);
        EXPECT_EQ(actual[index].Source.Reference, expected[index].Source.Reference);
        EXPECT_EQ(actual[index].Source.State, expected[index].Source.State);
        EXPECT_EQ(actual[index].Source.Identity, expected[index].Source.Identity);
        EXPECT_EQ(actual[index].Source.Failure.has_value(), expected[index].Source.Failure.has_value());
        if (actual[index].Source.Failure) {
            EXPECT_EQ(actual[index].Source.Failure->Message, expected[index].Source.Failure->Message);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Workers, ResourceReadiness, testing::Values(1U, 2U, 8U));

} // namespace
