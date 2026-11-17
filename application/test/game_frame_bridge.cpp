#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

#include <application/runtime/frame_loop.h>
#include <application/runtime/game_frame_bridge.h>
#include <gtest/gtest.h>
#include <renderer/render_components.h>

namespace {

using Clock = std::chrono::steady_clock;

template<typename Predicate>
bool Eventually(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds{5};
    while (!predicate()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

struct HeldFrame {
    NApplication::NRuntime::ScenePublication Publication;
    const NRenderer::RenderWorld* Snapshot = nullptr;
    std::promise<void> Completion;
    bool Complete = false;
};

struct Consumer {
    const std::thread::id Owner = std::this_thread::get_id();
    std::mutex Mutex;
    std::vector<std::shared_ptr<HeldFrame>> Frames;
    bool Drained = false;

    void Consume(const NEngine::NRuntime::FrameContext& frame,
                 const NApplication::NRuntime::ScenePublication& publication,
                 const NRenderer::RenderWorld& snapshot,
                 const NRenderer::RenderPlan& plan) {
        EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
        EXPECT_NE(std::this_thread::get_id(), Owner);
        EXPECT_EQ(frame.GetFrame().GetGeneration(), publication.Frame.GetGeneration());
        EXPECT_EQ(frame.GetFrame().GetFrameIndex(), publication.Frame.GetFrameSequence());
        EXPECT_EQ(plan.GetFrame(), snapshot.GetFrame());
        auto held = std::make_shared<HeldFrame>();
        held->Publication = publication;
        held->Snapshot = &snapshot;
        // Completion registration precedes publishing the intent to the owner-thread consumer.
        frame.RegisterGpuCompletion(held->Completion.get_future().share());
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
        const auto held = Get(index);
        if (!held->Complete) {
            held->Snapshot = nullptr;
            held->Complete = true;
            held->Completion.set_value();
        }
    }
    void Drain() {
        EXPECT_EQ(std::this_thread::get_id(), Owner);
        for (std::size_t i = 0; i < Count(); ++i) {
            Complete(i);
        }
        Drained = true;
    }
};

class Callbacks: public NApplication::NRuntime::IFrameLoopCallbacks {
public:
    void CaptureInput() override {
        EXPECT_EQ(std::this_thread::get_id(), Owner);
        ++Captures;
    }
    void OnUpdate() override {
        EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
        ++Updates;
    }
    void OnPostUpdate() override {
        EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
        ++Posts;
    }
    bool IsShutdownRequested() const override {
        return Shutdown || Posts.load() >= Target;
    }
    const std::thread::id Owner = std::this_thread::get_id();
    int Captures = 0;
    std::atomic<int> Updates = 0;
    std::atomic<int> Posts = 0;
    bool Shutdown = false;
    int Target = std::numeric_limits<int>::max();
};

class WorldCallbacks final: public Callbacks {
public:
    explicit WorldCallbacks(NEcs::World& world)
        : World(world)
        , Camera(world.CreateEntity()) {
        World.AddComponent<NRenderer::TransformComponent>(Camera);
        World.AddComponent<NRenderer::CameraComponent>(Camera);
        World.RegisterSystem(
                {NEcs::Write<NRenderer::TransformComponent>()},
                [this](NEcs::World& source) {
                    EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
                    ++source.GetComponent<NRenderer::TransformComponent>(Camera).Value.Translation.X;
                },
                NEcs::SystemSchedule{.Id = 1});
    }
    void OnUpdate() override {
        Callbacks::OnUpdate();
        World.GetComponent<NRenderer::TransformComponent>(Camera).Value.Translation.X =
                static_cast<float>((Updates.load() - 1) * 10);
    }
    void OnPostUpdate() override {
        Callbacks::OnPostUpdate();
        auto& x = World.GetComponent<NRenderer::TransformComponent>(Camera).Value.Translation.X;
        EXPECT_FLOAT_EQ(x, static_cast<float>((Updates.load() - 1) * 10 + 1));
        x += 2;
        World.BeginDeferredStructuralPhase({{.Order = 0, .EntityCreateCount = 1}});
        auto commands = World.CreateDeferredStructuralCommandBuffer(0);
        const auto entity = World.DeferCreateEntity(commands);
        NEcs::World::DeferAddComponent<NRenderer::TransformComponent>(commands, entity);
        NEcs::World::DeferAddComponent<NRenderer::CameraComponent>(commands, entity);
        World.SubmitDeferredStructuralCommands(std::move(commands));
    }
    NEcs::World& World;
    NEcs::Entity Camera;
};

std::unique_ptr<NApplication::NRuntime::GameFrameBridge>
MakeBridge(NScene::SceneManager& scenes,
           NResources::ResourceManager& resources,
           Consumer& consumer,
           NApplication::NRuntime::GameFrameBridge::Publication publish = {}) {
    return std::make_unique<NApplication::NRuntime::GameFrameBridge>(
            scenes,
            resources,
            [&consumer](const auto& frame, const auto& publication, const auto& snapshot, const auto& plan) {
                consumer.Consume(frame, publication, snapshot, plan);
            },
            std::move(publish),
            [&consumer] { consumer.Drain(); });
}

NApplication::ApplicationConfig Config(std::size_t workers, std::size_t slots) {
    return {.Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
            .MaxActiveFrames = slots,
            .WorkerCount = workers};
}

bool Advance(NApplication::NRuntime::FrameLoop& loop, Callbacks& callbacks, Consumer& consumer, std::size_t count) {
    callbacks.Target = static_cast<int>(count);
    const bool reached = Eventually([&] {
        (void)loop.Step(callbacks);
        if (consumer.Count() >= count) {
            return true;
        }
        loop.WaitForProgress();
        return false;
    });
    callbacks.Target = std::numeric_limits<int>::max();
    return reached;
}

TEST(GameFrameBridge, WorkerSlotMatrixJoinsUpdatesCommitsAndBoundsImmutableConsumers) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        for (const std::size_t slots: {1U, 2U, 3U}) {
            SCOPED_TRACE(testing::Message() << workers << " workers, " << slots << " slots");
            NResources::ResourceManager resources;
            NScene::SceneManager scenes;
            const auto scene = scenes.CreateScene("matrix");
            scenes.Activate(scene);
            WorldCallbacks callbacks{scenes.Acquire(scene)->GetWorld()};
            Consumer consumer;
            int publications = 0;
            NApplication::NRuntime::FrameLoop loop{Config(workers, slots),
                                                   MakeBridge(scenes, resources, consumer, [&](auto&) {
                                                       EXPECT_EQ(std::this_thread::get_id(), consumer.Owner);
                                                       ++publications;
                                                   })};
            loop.Start();
            ASSERT_TRUE(Advance(loop, callbacks, consumer, slots));
            for (int retry = 0; retry < 20; ++retry) {
                (void)loop.Step(callbacks);
            }
            EXPECT_EQ(publications, slots);
            EXPECT_EQ(callbacks.Captures, slots);
            EXPECT_EQ(callbacks.Updates.load(), slots);
            EXPECT_EQ(callbacks.Posts.load(), slots);
            for (std::size_t i = 0; i < slots; ++i) {
                const auto held = consumer.Get(i);
                EXPECT_EQ(held->Publication.Scene, scene);
                ASSERT_NE(held->Snapshot, nullptr);
                ASSERT_EQ(held->Snapshot->GetViews().size(), i + 2);
                EXPECT_FLOAT_EQ(held->Snapshot->GetViews()[0].WorldTransform.Translation.X,
                                static_cast<float>(i * 10 + 3));
            }
            consumer.Complete(0);
            ASSERT_TRUE(Advance(loop, callbacks, consumer, slots + 1));
            EXPECT_EQ(publications, slots + 1);
            ASSERT_EQ(consumer.Get(slots)->Snapshot->GetViews().size(), slots + 2);
            // The old GPU slots remain immutable while a recycled slot uses the new World state.
            for (std::size_t i = 1; i < slots; ++i) {
                EXPECT_EQ(consumer.Get(i)->Snapshot->GetViews().size(), i + 2);
            }
            loop.Stop();
            loop.CheckError();
            EXPECT_TRUE(consumer.Drained);
            EXPECT_EQ(loop.GetDiagnostics().Recycled, slots + 1);
        }
    }
}

TEST(GameFrameBridge, EmptySwitchUnloadAndRestartPublishNewFrameIdentity) {
    NResources::ResourceManager resources;
    NScene::SceneManager scenes;
    Consumer consumer;
    Callbacks callbacks;
    int publications = 0;
    NApplication::NRuntime::FrameLoop loop{Config(2, 3),
                                           MakeBridge(scenes, resources, consumer, [&](auto&) { ++publications; })};
    loop.Start();
    ASSERT_TRUE(Advance(loop, callbacks, consumer, 1));
    EXPECT_FALSE(consumer.Get(0)->Publication.Scene);
    EXPECT_TRUE(consumer.Get(0)->Snapshot->GetViews().empty());
    const auto oldScene = scenes.CreateScene("old");
    scenes.Activate(oldScene);
    const std::weak_ptr<NScene::Scene> lifetime = scenes.Acquire(oldScene);
    ASSERT_TRUE(Advance(loop, callbacks, consumer, 2));
    scenes.Unload(oldScene);
    const auto replacement = scenes.CreateScene("replacement");
    scenes.Activate(replacement);
    ASSERT_TRUE(Advance(loop, callbacks, consumer, 3));
    EXPECT_TRUE(lifetime.expired());
    EXPECT_EQ(consumer.Get(1)->Publication.Scene, oldScene);
    EXPECT_EQ(consumer.Get(1)->Publication.Name, "old");
    EXPECT_EQ(consumer.Get(2)->Publication.Scene, replacement);
    EXPECT_NE(oldScene, replacement);
    loop.Stop();
    loop.CheckError();
    const auto previous = consumer.Get(0)->Publication.Frame;
    loop.Start();
    ASSERT_TRUE(Advance(loop, callbacks, consumer, 4));
    EXPECT_NE(consumer.Get(3)->Publication.Frame.GetRunGeneration(), previous.GetRunGeneration());
    EXPECT_EQ(consumer.Get(3)->Publication.Scene, replacement);
    loop.Stop();
    loop.CheckError();
    EXPECT_EQ(publications, 4);
}

TEST(GameFrameBridge, PublicationFailureRollsBackBeforeCallbacksAndShutdownClosesAdmission) {
    NResources::ResourceManager resources;
    NScene::SceneManager scenes;
    Consumer consumer;
    Callbacks callbacks;
    bool fail = true;
    int publications = 0;
    NApplication::NRuntime::FrameLoop loop{Config(1, 1), MakeBridge(scenes, resources, consumer, [&](auto&) {
                                               ++publications;
                                               if (fail) {
                                                   throw std::runtime_error{"publication"};
                                               }
                                           })};
    loop.Start();
    EXPECT_THROW((void)loop.Step(callbacks), std::runtime_error);
    EXPECT_EQ(callbacks.Captures, 0);
    EXPECT_EQ(callbacks.Updates.load(), 0);
    fail = false;
    ASSERT_TRUE(Advance(loop, callbacks, consumer, 1));
    consumer.Complete(0);
    callbacks.Shutdown = true;
    for (int retry = 0; retry < 20; ++retry) {
        (void)loop.Step(callbacks);
    }
    EXPECT_EQ(publications, 2);
    EXPECT_EQ(callbacks.Updates.load(), 1);
    EXPECT_EQ(callbacks.Posts.load(), 1);
    loop.Stop();
    loop.CheckError();
}

TEST(GameFrameBridge, AsyncEcsFailureSuppressesPostUpdateExtractionAndConsumer) {
    NResources::ResourceManager resources;
    NScene::SceneManager scenes;
    Consumer consumer;
    Callbacks callbacks;
    const auto scene = scenes.CreateScene("failure");
    scenes.Activate(scene);
    scenes.Acquire(scene)->GetWorld().RegisterSystem(
            {},
            [](NEcs::World&) { throw std::runtime_error{"ECS"}; },
            NEcs::SystemSchedule{.Id = 1});
    NApplication::NRuntime::FrameLoop loop{Config(1, 3), MakeBridge(scenes, resources, consumer)};
    loop.Start();
    bool failed = false;
    ASSERT_TRUE(Eventually([&] {
        try {
            (void)loop.Step(callbacks);
            loop.WaitForProgress();
        } catch (...) {
            failed = true;
        }
        return failed;
    }));
    loop.Stop();
    EXPECT_ANY_THROW(loop.CheckError());
    EXPECT_EQ(callbacks.Updates.load(), 1);
    EXPECT_EQ(callbacks.Posts.load(), 0);
    EXPECT_EQ(consumer.Count(), 0U);
}

TEST(GameFrameBridge, StopCancelsAsyncUpdateAndRetainsUnloadedSceneUntilCpuQuiescence) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        NResources::ResourceManager resources;
        NScene::SceneManager scenes;
        Consumer consumer;
        Callbacks callbacks;
        const auto scene = scenes.CreateScene("cancel");
        scenes.Activate(scene);
        const std::weak_ptr<NScene::Scene> lifetime = scenes.Acquire(scene);
        std::binary_semaphore release{0};
        std::atomic_bool entered = false;
        scenes.Acquire(scene)->GetWorld().RegisterSystem(
                {},
                [&](NEcs::World&) {
                    entered = true;
                    EXPECT_TRUE(release.try_acquire_for(std::chrono::seconds{5}));
                },
                NEcs::SystemSchedule{.Id = 1});
        NApplication::NRuntime::FrameLoop loop{Config(workers, 3), MakeBridge(scenes, resources, consumer)};
        loop.Start();
        ASSERT_TRUE(Eventually([&] {
            (void)loop.Step(callbacks);
            return entered.load();
        }));
        scenes.Unload(scene);
        EXPECT_FALSE(lifetime.expired());
        // Stop remains on the owner thread. A helper only releases the already running system.
        std::thread finish{[&] {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            EXPECT_FALSE(lifetime.expired());
            release.release();
        }};
        loop.Stop();
        finish.join();
        loop.CheckError();
        EXPECT_TRUE(lifetime.expired());
        EXPECT_EQ(callbacks.Updates.load(), 1);
        EXPECT_EQ(callbacks.Posts.load(), 0);
        EXPECT_EQ(consumer.Count(), 0U);
        EXPECT_TRUE(consumer.Drained);
    }
}

TEST(GameFrameBridge, RejectsUnboundAndForeignEnginesAndDuplicateComposition) {
    NResources::ResourceManager resources;
    NScene::SceneManager scenes;
    Consumer consumer;
    auto bridge = MakeBridge(scenes, resources, consumer);
    NEngine::Engine foreign{{1, 1}};
    EXPECT_ANY_THROW((void)bridge->TryBeginFrame(foreign));
    auto engine = bridge->Create({1, 1});
    EXPECT_ANY_THROW((void)bridge->Create({1, 1}));
    EXPECT_ANY_THROW((void)bridge->TryBeginFrame(foreign));
    engine->Start();
    std::thread wrongThread{[&] { EXPECT_ANY_THROW((void)bridge->TryBeginFrame(*engine)); }};
    wrongThread.join();
    const auto stale = bridge->TryBeginFrame(*engine);
    ASSERT_TRUE(stale);
    engine->Stop();
    engine->Start();
    const auto current = bridge->TryBeginFrame(*engine);
    ASSERT_TRUE(current);
    EXPECT_NE(stale->GetRunGeneration(), current->GetRunGeneration());
    EXPECT_FALSE(engine->CompleteUserUpdate(*stale));
    EXPECT_FALSE(engine->AbortFrame(*stale));
    EXPECT_FALSE(engine->IsUpdateComplete(*stale));
    ASSERT_TRUE(engine->CompleteUserUpdate(*current));
    ASSERT_TRUE(engine->Draw());
    ASSERT_TRUE(Eventually([&] { return consumer.Count() == 1; }));
    engine->Stop();
    EXPECT_FALSE(engine->GetLastError());
}

} // namespace
