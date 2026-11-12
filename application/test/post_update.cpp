#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <application/runtime/frame_loop.h>
#include <ecs/world.h>
#include <engine/runtime/frame_runtime.h>
#include <engine/runtime/world_update.h>
#include <gtest/gtest.h>
#include <renderer/extraction.h>
#include <renderer/render_components.h>

namespace {

class WorldRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    WorldRuntime() {
        World->AddComponent<NRenderer::TransformComponent>(Camera);
        World->AddComponent<NRenderer::CameraComponent>(Camera);
        World->RegisterSystem(
                {NEcs::Write<NRenderer::TransformComponent>()},
                [camera = Camera](NEcs::World& world) {
                    std::this_thread::yield();
                    ++world.GetComponent<NRenderer::TransformComponent>(camera).Value.Translation.X;
                },
                NEcs::SystemSchedule{.Id = 1});
    }
    void Update(const NEngine::NRuntime::FrameContext&) override {
        ADD_FAILURE() << "Scheduled ECS update was bypassed";
    }
    void ScheduleUpdate(const NEngine::NRuntime::FrameContext&,
                        NCommon::TaskSystem& tasks,
                        std::shared_ptr<NEngine::NRuntime::UpdateCompletion> completion) override {
        NEngine::NRuntime::ScheduleWorldUpdate(World, tasks, completion);
    }
    [[nodiscard]] bool HasPostUpdate() const noexcept override {
        return true;
    }
    NCommon::TaskHandle SchedulePostUpdate(const NEngine::NRuntime::FrameContext&, NCommon::TaskContext&) override {
        World->BeginDeferredStructuralPhase({{.Order = 0, .EntityCreateCount = 1}});
        return {};
    }
    void FinalizePostUpdate(const NEngine::NRuntime::FrameContext&) override {
        World->ApplyDeferredStructuralChanges();
    }
    NCommon::TaskHandle ScheduleExtract(const NEngine::NRuntime::FrameContext& frame,
                                        NCommon::TaskContext& task) override {
        const auto slot = frame.GetFrame().GetSlotIndex();
        return NRenderer::NInternal::ScheduleRenderWorldExtraction(
                *World,
                Resources,
                frame.GetStorage(),
                task,
                [this, slot](const NRenderer::RenderWorld& snapshot) {
                    Snapshots[slot] = &snapshot;
                    ++Extractions;
                },
                1);
    }
    void Draw(const NEngine::NRuntime::FrameContext& frame) override {
        // Only frame-owned immutable data: the next user update may already be mutating World.
        const auto index = frame.GetFrame().GetFrameIndex();
        const auto& snapshot = *Snapshots[frame.GetFrame().GetSlotIndex()];
        EXPECT_EQ(snapshot.GetViews().size(), index + 2);
        bool foundCamera = false;
        for (const auto& view: snapshot.GetViews()) {
            if (view.Id.Index == Camera.Index) {
                foundCamera = true;
                EXPECT_FLOAT_EQ(view.WorldTransform.Translation.X, static_cast<float>(index * 10 + 3));
            }
        }
        EXPECT_TRUE(foundCamera);
        ++Draws;
    }

    std::shared_ptr<NEcs::World> World = std::make_shared<NEcs::World>();
    NEcs::Entity Camera = World->CreateEntity();
    NResources::ResourceManager Resources;
    const NRenderer::RenderWorld* Snapshots[3]{};
    std::atomic<int> Extractions{0};
    std::atomic<int> Draws{0};
};

class WorldFactory final: public NApplication::NRuntime::IEngineFactory {
public:
    std::unique_ptr<NEngine::Engine> Create(NEngine::EngineConfig config) override {
        auto runtime = std::make_unique<WorldRuntime>();
        Runtime = runtime.get();
        return NEngine::NRuntime::EngineFactory::Create(config, std::move(runtime));
    }
    WorldRuntime* Runtime = nullptr;
};

class WorldCallbacks final: public NApplication::NRuntime::IFrameLoopCallbacks {
public:
    explicit WorldCallbacks(WorldRuntime& runtime)
        : m_runtime(runtime) {
    }
    void OnUpdate() override {
        EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
        EXPECT_EQ(m_runtime.Extractions.load(), Updates);
        m_runtime.World->GetComponent<NRenderer::TransformComponent>(m_runtime.Camera).Value.Translation.X =
                static_cast<float>(Updates * 10);
        ++Updates;
    }
    void OnPostUpdate() override {
        EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
        EXPECT_EQ(m_runtime.Extractions.load(), Updates - 1);
        auto& world = *m_runtime.World;
        auto& transform = world.GetComponent<NRenderer::TransformComponent>(m_runtime.Camera).Value;
        EXPECT_FLOAT_EQ(transform.Translation.X, static_cast<float>((Updates - 1) * 10 + 1));
        transform.Translation.X += 2;
        auto commands = world.CreateDeferredStructuralCommandBuffer(0);
        const auto entity = world.DeferCreateEntity(commands);
        NEcs::World::DeferAddComponent<NRenderer::TransformComponent>(commands, entity);
        NEcs::World::DeferAddComponent<NRenderer::CameraComponent>(commands, entity);
        world.SubmitDeferredStructuralCommands(std::move(commands));
        ++Posts;
    }
    int Updates = 0;
    int Posts = 0;

private:
    WorldRuntime& m_runtime;
};

TEST(ApplicationPostUpdate, EcsCompletionUserWritesAndDeferredCommitReachParallelRenderWorldExtraction) {
    for (std::size_t workers: {1U, 2U}) {
        for (std::size_t slots: {1U, 2U, 3U}) {
            auto factory = std::make_unique<WorldFactory>();
            auto& host = *factory;
            NApplication::NRuntime::FrameLoop loop{{.Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
                                                    .MaxActiveFrames = slots,
                                                    .WorkerCount = workers},
                                                   std::move(factory)};
            WorldCallbacks callbacks{*host.Runtime};
            loop.Start();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (host.Runtime->Draws.load() < 12 && std::chrono::steady_clock::now() < deadline) {
                if (!loop.Step(callbacks)) {
                    loop.WaitForProgress();
                }
            }
            loop.Stop();
            loop.CheckError();
            EXPECT_GE(host.Runtime->Draws.load(), 12);
            EXPECT_GE(callbacks.Posts, 12);
            EXPECT_GE(callbacks.Updates, callbacks.Posts);
        }
    }
}

} // namespace
