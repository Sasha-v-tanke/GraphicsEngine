#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

#include <ecs/world.h>
#include <engine/runtime/frame_runtime.h>
#include <engine/runtime/world_update.h>
#include <gtest/gtest.h>

namespace {
using namespace std::chrono_literals;

struct Counter {
    int Value = 0;
};

class AsyncWorldRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    std::shared_ptr<NEcs::World> World = std::make_shared<NEcs::World>();
    NEcs::Entity Entity = World->CreateEntity();
    std::atomic<int> Extracts{0};
    std::atomic<int> Draws{0};
    AsyncWorldRuntime() {
        World->AddComponent<Counter>(Entity);
        World->RegisterSystem(
                {NEcs::Write<Counter>()},
                [entity = Entity](NEcs::World& world) { ++world.GetComponent<Counter>(entity).Value; },
                NEcs::SystemSchedule{.Id = 1});
    }
    void Update(const NEngine::NRuntime::FrameContext&) override {
        ADD_FAILURE() << "Async adapter was bypassed";
    }
    void ScheduleUpdate(const NEngine::NRuntime::FrameContext&,
                        NCommon::TaskSystem& tasks,
                        std::shared_ptr<NEngine::NRuntime::UpdateCompletion> completion) override {
        NEngine::NRuntime::ScheduleWorldUpdate(World, tasks, completion);
    }
    void Extract(const NEngine::NRuntime::FrameContext& frame) override {
        EXPECT_EQ(World->GetComponent<Counter>(Entity).Value, frame.GetFrame().GetFrameIndex() + 1);
        ++Extracts;
    }
    void Draw(const NEngine::NRuntime::FrameContext&) override {
        ++Draws;
    }
};

template<typename Predicate>
bool Eventually(NEngine::Engine& engine, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto version = engine.GetProgressVersion();
        if (predicate()) {
            return true;
        }
        if (engine.GetLastError()) {
            return false;
        }
        engine.WaitForProgress(version, 1ms);
    }
    return false;
}

TEST(EngineAsyncUpdate, SystemsCompleteBeforeDrawAndNextMutableUpdateAcrossWorkerAndSlotMatrix) {
    for (std::size_t workers: {1U, 2U, 8U}) {
        for (std::size_t slots: {1U, 2U, 3U}) {
            auto runtime = std::make_unique<AsyncWorldRuntime>();
            auto& observed = *runtime;
            auto engine = NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = slots, .WorkerCount = workers},
                                                                   std::move(runtime));
            engine->Start();
            for (int frame = 0; frame < 12; ++frame) {
                std::optional<NEngine::FrameKey> key;
                ASSERT_TRUE(Eventually(*engine, [&] {
                    key = engine->TryBeginFrame();
                    return key.has_value();
                }));
                ASSERT_TRUE(engine->ScheduleUserUpdate(*key, [&, frame] {
                    EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
                    EXPECT_EQ(observed.Extracts.load(), frame);
                    EXPECT_EQ(observed.World->GetComponent<Counter>(observed.Entity).Value, frame);
                }));
                ASSERT_TRUE(Eventually(*engine, [&] { return engine->IsUpdateComplete(*key); }));
                EXPECT_EQ(observed.World->GetComponent<Counter>(observed.Entity).Value, frame + 1);
                ASSERT_TRUE(engine->Draw());
            }
            engine->Stop();
            EXPECT_EQ(observed.Extracts.load(), 12);
            EXPECT_EQ(observed.Draws.load(), 12);
            EXPECT_FALSE(engine->GetLastError());
        }
    }
}

TEST(EngineAsyncUpdate, StopBeforeCancellationBindingIsDeliveredAndWaitsForSemanticQuiescence) {
    class DelayedRuntime final: public NEngine::NRuntime::IFrameRuntime {
    public:
        std::promise<void> Entered;
        std::promise<void> Release;
        std::shared_future<void> Released = Release.get_future().share();
        std::atomic<bool> Cancelled{false};
        void Update(const NEngine::NRuntime::FrameContext&) override {
        }
        void ScheduleUpdate(const NEngine::NRuntime::FrameContext&,
                            NCommon::TaskSystem&,
                            std::shared_ptr<NEngine::NRuntime::UpdateCompletion> completion) override {
            Entered.set_value();
            Released.wait();
            completion->BindCancellation([this] { Cancelled.store(true); });
            EXPECT_TRUE(completion->IsStopRequested());
            completion->Finish({.Status = NEngine::NRuntime::EUpdateStatus::CANCELLED});
        }
        void Draw(const NEngine::NRuntime::FrameContext&) override {
            ADD_FAILURE();
        }
    };
    auto runtime = std::make_unique<DelayedRuntime>();
    auto& observed = *runtime;
    auto entered = observed.Entered.get_future();
    auto engine = NEngine::NRuntime::EngineFactory::Create({.WorkerCount = 1}, std::move(runtime));
    engine->Start();
    ASSERT_TRUE(engine->Update());
    EXPECT_EQ(entered.wait_for(2s), std::future_status::ready);
    std::promise<void> stopped;
    auto future = stopped.get_future();
    std::jthread stopper{[&] {
        engine->Stop();
        stopped.set_value();
    }};
    while (engine->GetState() != NEngine::EEngineState::STOPPING) {
        std::this_thread::yield();
    }
    EXPECT_EQ(future.wait_for(5ms), std::future_status::timeout);
    observed.Release.set_value();
    EXPECT_EQ(future.wait_for(2s), std::future_status::ready);
    stopper.join();
    EXPECT_TRUE(observed.Cancelled.load());
    EXPECT_EQ(engine->GetState(), NEngine::EEngineState::STOPPED);
}

TEST(EngineAsyncUpdate, SystemFailurePreventsDrawAndPropagatesRuntimeError) {
    auto runtime = std::make_unique<AsyncWorldRuntime>();
    auto& observed = *runtime;
    observed.World->RegisterSystem(
            {},
            [](NEcs::World&) { throw std::runtime_error{"async ECS failure"}; },
            NEcs::SystemSchedule{.Id = 2, .After = {1}});
    auto engine = NEngine::NRuntime::EngineFactory::Create({.WorkerCount = 1}, std::move(runtime));
    engine->Start();
    ASSERT_TRUE(engine->Update());
    ASSERT_TRUE(engine->Draw());
    ASSERT_TRUE(Eventually(*engine, [&] { return engine->GetLastError().has_value(); }));
    EXPECT_EQ(engine->GetLastError()->Message, "async ECS failure");
    engine->Stop();
    EXPECT_EQ(observed.Extracts.load(), 0);
    EXPECT_EQ(observed.Draws.load(), 0);
}
} // namespace

namespace {
TEST(EngineAsyncUpdate, StopWaitsForProducerTicketEvenWhenExecutorIsIdle) {
    class ProducerRuntime final: public NEngine::NRuntime::IFrameRuntime {
    public:
        std::shared_ptr<NEcs::World> World = std::make_shared<NEcs::World>();
        std::promise<NEcs::World::DeferredStructuralCommandBuffer> Produced;
        std::atomic<int> RetiredFrameObjects{0};
        struct FrameProbe {
            explicit FrameProbe(std::atomic<int>& retired)
                : Retired(retired) {
            }
            ~FrameProbe() {
                ++Retired;
            }
            std::atomic<int>& Retired;
        };
        ProducerRuntime() {
            World->BeginDeferredStructuralPhase({{.Order = 0, .SystemOrder = 10}});
            World->RegisterSystem(
                    {},
                    [this](NEcs::World& world) { Produced.set_value(world.CreateDeferredStructuralCommandBuffer(0)); },
                    NEcs::SystemSchedule{.Id = 1},
                    10);
        }
        void Update(const NEngine::NRuntime::FrameContext&) override {
            ADD_FAILURE();
        }
        void ScheduleUpdate(const NEngine::NRuntime::FrameContext& frame,
                            NCommon::TaskSystem& tasks,
                            std::shared_ptr<NEngine::NRuntime::UpdateCompletion> completion) override {
            static_cast<void>(frame.GetStorage().Emplace<FrameProbe>(RetiredFrameObjects));
            NEngine::NRuntime::ScheduleWorldUpdate(World, tasks, completion);
        }
        void Draw(const NEngine::NRuntime::FrameContext&) override {
            ADD_FAILURE();
        }
    };
    auto runtime = std::make_unique<ProducerRuntime>();
    auto& observed = *runtime;
    auto produced = observed.Produced.get_future();
    auto engine =
            NEngine::NRuntime::EngineFactory::Create({.MaxActiveFrames = 1, .WorkerCount = 1}, std::move(runtime));
    engine->Start();
    const auto key = engine->TryBeginFrame();
    ASSERT_TRUE(key);
    EXPECT_TRUE(engine->CompleteUserUpdate(*key));
    ASSERT_EQ(produced.wait_for(2s), std::future_status::ready);
    auto buffer = std::make_optional(produced.get());
    EXPECT_FALSE(engine->IsUpdateComplete(*key));
    std::promise<void> stopped;
    auto future = stopped.get_future();
    std::jthread stopper{[&] {
        engine->Stop();
        stopped.set_value();
    }};
    EXPECT_EQ(future.wait_for(10ms), std::future_status::timeout);
    EXPECT_EQ(observed.RetiredFrameObjects.load(), 0);
    buffer.reset();
    EXPECT_EQ(future.wait_for(2s), std::future_status::ready);
    stopper.join();
    EXPECT_EQ(observed.RetiredFrameObjects.load(), 1);
    EXPECT_FALSE(engine->GetLastError());
}
} // namespace
