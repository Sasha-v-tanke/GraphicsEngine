#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include <application/runtime/scene_runtime.h>
#include <graphics/material.h>
#include <gtest/gtest.h>
#include <renderer/render_components.h>
#include <resources/async_resource_loader.h>
#include <resources/mesh_loader.h>

namespace {

template<typename Predicate>
bool Eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

struct ReadGate {
    std::mutex Mutex;
    std::condition_variable_any Wake;
    std::atomic_bool Entered = false;
    bool Open = false;

    NResources::ResourceIoBytes Read(const std::stop_token& stop) {
        Entered = true;
        std::unique_lock lock{Mutex};
        Wake.wait(lock, stop, [&] { return Open; });
        const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        const auto bytes = std::as_bytes(std::span{obj});
        return {bytes.begin(), bytes.end()};
    }
    void Release() {
        std::lock_guard lock{Mutex};
        Open = true;
        Wake.notify_all();
    }
};

struct HeldFrame {
    NApplication::NRuntime::ScenePublication Publication;
    const NRenderer::RenderWorld* Snapshot = nullptr;
    std::size_t Commands = 0;
    std::promise<void> Gpu;
    bool Completed = false;
};

struct GpuConsumer {
    std::mutex Mutex;
    std::vector<std::shared_ptr<HeldFrame>> Frames;

    void Consume(const NEngine::NRuntime::FrameContext& frame,
                 const NApplication::NRuntime::ScenePublication& publication,
                 const NRenderer::RenderWorld& snapshot,
                 const NRenderer::RenderPlan& plan) {
        auto held = std::make_shared<HeldFrame>();
        held->Publication = publication;
        held->Snapshot = &snapshot;
        held->Commands = plan.GetDrawCommands().size();
        frame.RegisterGpuCompletion(held->Gpu.get_future().share());
        std::lock_guard lock{Mutex};
        Frames.push_back(std::move(held));
    }
    std::size_t Count() {
        std::lock_guard lock{Mutex};
        return Frames.size();
    }
    std::shared_ptr<HeldFrame> Get(std::size_t index) {
        std::lock_guard lock{Mutex};
        return Frames.at(index);
    }
    void Complete(std::size_t index) {
        std::lock_guard lock{Mutex};
        const auto& frame = Frames.at(index);
        if (!frame->Completed) {
            frame->Snapshot = nullptr;
            frame->Completed = true;
            frame->Gpu.set_value();
        }
    }
    void Drain() {
        for (std::size_t i = 0; i < Count(); ++i) {
            Complete(i);
        }
    }
};

class SceneSwitching: public testing::TestWithParam<std::size_t> {};

TEST_P(SceneSwitching, DelayedIoSwitchUnloadAndEmptyStateWithThreeActiveSlots) {
    NResources::ResourceManager resources;
    NCommon::TaskSystem ioTasks{GetParam()};
    ReadGate gate;
    NResources::AsyncResourceLoader loader{resources, ioTasks, {}, [&](const auto&, const auto& stop, auto) {
                                               return gate.Read(stop);
                                           }};
    const auto delayed = loader.LoadMesh({"mesh", "delayed"}, "delayed.obj");
    ASSERT_TRUE(Eventually([&] { return gate.Entered.load(); }));
    const auto shared = resources.Request<NResources::MeshData>({"mesh", "shared"});
    const auto bytes = [] {
        const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        return NResources::DecodeMesh(std::as_bytes(std::span{obj}));
    }();
    resources.PublishReady(resources.BeginLoading(shared), bytes);
    const auto material = resources.Request<NGraphics::Material>({"material", "shared"});
    resources.PublishReady(resources.BeginLoading(material),
                           std::make_shared<NGraphics::Material>(NResources::ResourceIdentity{"pipeline", "test"}));
    NScene::SceneManager scenes;
    const auto makeScene = [&](const char* name) {
        const auto handle = scenes.CreateScene(name);
        auto& world = scenes.Acquire(handle)->GetWorld();
        const auto camera = world.CreateEntity();
        world.AddComponent<NRenderer::TransformComponent>(camera);
        world.AddComponent<NRenderer::CameraComponent>(camera);
        for (const auto mesh: {shared, delayed}) {
            const auto object = world.CreateEntity();
            world.AddComponent<NRenderer::TransformComponent>(object);
            world.AddComponent<NRenderer::RenderableComponent>(object, NRenderer::RenderableComponent{mesh, material});
        }
        return handle;
    };
    const auto firstScene = makeScene("first");
    const auto secondScene = makeScene("second");
    const std::weak_ptr<NScene::Scene> firstLifetime = scenes.Acquire(firstScene);
    GpuConsumer gpu;
    auto runtime = std::make_unique<NApplication::NRuntime::SceneRuntime>(
            resources,
            [&](const auto& frame, const auto& publication, const auto& snapshot, const auto& plan) {
                gpu.Consume(frame, publication, snapshot, plan);
            },
            [&] { gpu.Drain(); });
    auto* host = runtime.get();
    auto engine = NEngine::NRuntime::EngineFactory::Create({3, GetParam()}, std::move(runtime));
    engine->Start();
    const auto admit = [&](const auto& publish, std::size_t expected) {
        std::optional<NEngine::FrameKey> frame;
        EXPECT_TRUE(Eventually([&] {
            frame = host->TryBeginFrame(*engine, scenes, publish);
            return frame.has_value();
        }));
        if (!frame) {
            return false;
        }
        EXPECT_TRUE(engine->CompleteUserUpdate(*frame));
        EXPECT_TRUE(engine->Draw());
        return Eventually([&] { return gpu.Count() == expected || engine->GetLastError().has_value(); }) &&
               !engine->GetLastError();
    };
    ASSERT_TRUE(admit([&](auto& registry) { registry.Activate(firstScene); }, 1));
    ASSERT_TRUE(admit([&](auto& registry) { registry.Activate(secondScene); }, 2));
    ASSERT_TRUE(admit([&](auto& registry) { registry.Unload(firstScene); }, 3));
    ASSERT_TRUE(Eventually([&] { return firstLifetime.expired(); }));
    EXPECT_FALSE(scenes.IsRegistered(firstScene));
    EXPECT_TRUE(resources.TryAcquire(shared));
    ASSERT_EQ(gpu.Get(0)->Commands, 1U);
    EXPECT_EQ(gpu.Get(0)->Publication.Scene, firstScene);
    EXPECT_EQ(gpu.Get(1)->Publication.Scene, secondScene);
    ASSERT_EQ(gpu.Get(0)->Snapshot->GetResourceDiagnostics().size(), 1U);
    EXPECT_EQ(gpu.Get(0)->Snapshot->GetResourceDiagnostics()[0].Source.State, NResources::EResourceState::QUEUED);
    bool published = false;
    EXPECT_FALSE(host->TryBeginFrame(*engine, scenes, [&](auto&) { published = true; }));
    EXPECT_FALSE(published);
    // Dedicated I/O is blocked, but every Engine worker has already made forward progress.
    gate.Release();
    ASSERT_TRUE(Eventually([&] {
        loader.PumpUploads();
        return resources.GetState(delayed) == NResources::EResourceState::READY;
    }));
    EXPECT_EQ(gpu.Get(0)->Commands, 1U);
    EXPECT_EQ(gpu.Get(0)->Snapshot->GetResourceDiagnostics()[0].Source.State, NResources::EResourceState::QUEUED);
    gpu.Complete(0);
    ASSERT_TRUE(admit([&](auto& registry) { registry.Activate(secondScene); }, 4));
    EXPECT_EQ(gpu.Get(3)->Commands, 2U);
    gpu.Complete(1);
    ASSERT_TRUE(admit([&](auto& registry) { registry.Unload(secondScene); }, 5));
    EXPECT_FALSE(scenes.GetActiveHandle());
    EXPECT_FALSE(gpu.Get(4)->Publication.Scene);
    EXPECT_EQ(gpu.Get(4)->Commands, 0U);
    EXPECT_TRUE(resources.TryAcquire(shared));
    // An older GPU slot remains a complete immutable scene after both registry entries were unloaded.
    EXPECT_EQ(gpu.Get(2)->Snapshot->GetObjects().size(), 1U);
    EXPECT_EQ(gpu.Get(2)->Publication.Name, "second");
    for (std::size_t i = 0; i < 20; ++i) {
        gpu.Complete(i + 2);
        ASSERT_TRUE(admit(
                [&](auto& registry) {
                    const auto scene = makeScene("replacement");
                    registry.Activate(scene);
                    registry.Unload(scene);
                },
                i + 6));
        EXPECT_EQ(gpu.Get(i + 5)->Commands, 0U);
    }
    engine->Stop();
    EXPECT_FALSE(engine->GetLastError());
    EXPECT_EQ(engine->GetGpuRecycledFrameCount(), 25U);
    loader.Shutdown();
}

TEST(SceneRuntime, RejectsForeignThreadAndUnboundFrames) {
    NResources::ResourceManager resources;
    NScene::SceneManager scenes;
    auto runtime = std::make_unique<NApplication::NRuntime::SceneRuntime>(
            resources,
            [](const auto&, const auto&, const auto&, const auto&) {});
    auto* host = runtime.get();
    auto engine = NEngine::NRuntime::EngineFactory::Create({2, 1}, std::move(runtime));
    engine->Start();
    std::thread foreign{[&] { EXPECT_ANY_THROW((void)host->TryBeginFrame(*engine, scenes)); }};
    foreign.join();
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(Eventually([&] { return engine->GetLastError().has_value(); }));
    engine->Stop();
}

TEST(SceneRuntime, PublicationWaitsForPreviousWorldUpdate) {
    NResources::ResourceManager resources;
    NScene::SceneManager scenes;
    const auto scene = scenes.CreateScene("updating");
    std::atomic_bool entered = false;
    std::promise<void> release;
    const auto released = release.get_future().share();
    scenes.Acquire(scene)->GetWorld().RegisterSystem(
            {},
            [&](NEcs::World&) {
                entered = true;
                EXPECT_EQ(released.wait_for(std::chrono::seconds{3}), std::future_status::ready);
            },
            NEcs::SystemSchedule{.Id = 1});
    GpuConsumer gpu;
    auto runtime = std::make_unique<NApplication::NRuntime::SceneRuntime>(
            resources,
            [&](const auto& frame, const auto& publication, const auto& snapshot, const auto& plan) {
                gpu.Consume(frame, publication, snapshot, plan);
            },
            [&] { gpu.Drain(); });
    auto* host = runtime.get();
    auto engine = NEngine::NRuntime::EngineFactory::Create({3, 1}, std::move(runtime));
    engine->Start();
    const auto first = host->TryBeginFrame(*engine, scenes, [&](auto& registry) { registry.Activate(scene); });
    ASSERT_TRUE(first);
    ASSERT_TRUE(engine->CompleteUserUpdate(*first));
    ASSERT_TRUE(engine->Draw());
    ASSERT_TRUE(Eventually([&] { return entered.load(); }));
    bool published = false;
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(host->TryBeginFrame(*engine, scenes, [&](auto&) { published = true; }));
    }
    EXPECT_FALSE(published);
    release.set_value();
    ASSERT_TRUE(Eventually([&] { return gpu.Count() == 1; }));
    std::optional<NEngine::FrameKey> second;
    ASSERT_TRUE(Eventually([&] {
        second = host->TryBeginFrame(*engine, scenes, [&](auto& registry) { registry.Unload(scene); });
        return second.has_value();
    }));
    ASSERT_TRUE(engine->CompleteUserUpdate(*second));
    ASSERT_TRUE(engine->Draw());
    ASSERT_TRUE(Eventually([&] { return gpu.Count() == 2; }));
    EXPECT_EQ(gpu.Get(0)->Publication.Scene, scene);
    EXPECT_FALSE(gpu.Get(1)->Publication.Scene);
    engine->Stop();
    EXPECT_FALSE(engine->GetLastError());
}

TEST(SceneRuntime, ThrowingPublicationAbortsReservationAndStopDrainsBoundScene) {
    NResources::ResourceManager resources;
    NScene::SceneManager scenes;
    auto runtime = std::make_unique<NApplication::NRuntime::SceneRuntime>(
            resources,
            [](const auto&, const auto&, const auto&, const auto&) {});
    auto* host = runtime.get();
    auto engine = NEngine::NRuntime::EngineFactory::Create({2, 1}, std::move(runtime));
    engine->Start();
    EXPECT_THROW((void)host->TryBeginFrame(*engine, scenes, [](auto&) { throw std::runtime_error{"publish"}; }),
                 std::runtime_error);
    const auto scene = scenes.CreateScene("retained");
    scenes.Activate(scene);
    const std::weak_ptr<NScene::Scene> lifetime = scenes.Acquire(scene);
    ASSERT_TRUE(host->TryBeginFrame(*engine, scenes));
    scenes.Unload(scene);
    EXPECT_FALSE(lifetime.expired());
    engine->Stop();
    EXPECT_TRUE(lifetime.expired());
    EXPECT_FALSE(engine->GetLastError());
}

INSTANTIATE_TEST_SUITE_P(Workers, SceneSwitching, testing::Values(1U, 4U));

} // namespace
