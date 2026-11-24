#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <application/application.h>
#include <application/runtime/game_output.h>
#include <graphics/backend/completion_access.h>
#include <gtest/gtest.h>
#include <renderer/render_components.h>
#include <tests/common/test_error.h>
#include <window/engine/engine.h>
#include <window/engine/factory.h>

namespace {

struct State {
    const std::thread::id Owner = std::this_thread::get_id();
    bool Close = false;
    bool CanRestart = true;
    bool WindowLive = false;
    bool Completed = true;
    bool OutputActive = false;
    bool Suspended = false;
    bool FailServiceStart = false;
    NWindow::WindowSize Size{160, 120};
    int WindowDestroyed = 0;
    int Created = 0;
    int Starts = 0;
    int Stops = 0;
    int Destroyed = 0;
    int FailStarts = 0;
    int NotReady = 0;
    std::size_t ConfigFrames = 0;
    std::vector<NRenderer::RenderFrameIdentity> Frames;
    std::vector<std::size_t> Views;
    std::function<void()> Events;
};
thread_local State* g_state = nullptr;

class TestWindow final: public NWindow::NEngine::IWindowEngine {
public:
    explicit TestWindow(State& state)
        : m_state(state) {
        m_state.WindowLive = true;
    }
    ~TestWindow() override {
        m_state.WindowLive = false;
        ++m_state.WindowDestroyed;
    }
    void AttachEventSink(NWindow::NEngine::IWindowEventSink&) override {
    }
    void SetTitle(std::string_view) override {
    }
    void SetSize(NWindow::WindowSize size) override {
        m_state.Size = size;
    }
    [[nodiscard]] NWindow::WindowSize GetSize() const override {
        return m_state.Size;
    }
    [[nodiscard]] NWindow::WindowSize GetFramebufferSize() const override {
        return m_state.Size;
    }
    [[nodiscard]] bool ShouldClose() const override {
        return m_state.Close;
    }
    void RequestClose() override {
        m_state.Close = true;
    }
    [[nodiscard]] bool ResetCloseRequest() override {
        EXPECT_EQ(std::this_thread::get_id(), m_state.Owner);
        if (!m_state.CanRestart) {
            return false;
        }
        m_state.Close = false;
        return true;
    }
    void ProcessEvents() override {
        if (m_state.Events) {
            m_state.Events();
        }
    }

private:
    State& m_state;
};

class TestOutput final: public NApplication::NRuntime::IGameOutput {
public:
    explicit TestOutput(State& state)
        : m_state(state) {
        ++state.Created;
    }
    ~TestOutput() override {
        EXPECT_TRUE(m_state.WindowLive);
        EXPECT_FALSE(m_state.OutputActive);
        ++m_state.Destroyed;
    }
    void Start() override {
        EXPECT_EQ(std::this_thread::get_id(), m_state.Owner);
        if (m_state.FailStarts > 0) {
            --m_state.FailStarts;
            throw std::runtime_error{"output startup"};
        }
        ++m_state.Starts;
        m_state.OutputActive = true;
        m_next = 0;
    }
    void SetPresentationSize(int width, int height) override {
        EXPECT_EQ(std::this_thread::get_id(), m_state.Owner);
        m_state.Suspended = width <= 0 || height <= 0;
    }
    NGraphics::FrameSubmissionResult TrySubmit(const NRenderer::RenderWorld& world,
                                               const NRenderer::RenderPlan& plan) override {
        EXPECT_EQ(std::this_thread::get_id(), m_state.Owner);
        EXPECT_FALSE(NCommon::TaskSystem::IsCurrentThreadWorker());
        EXPECT_EQ(plan.GetFrame(), world.GetFrame());
        EXPECT_EQ(plan.GetFrame().ApplicationFrameIndex, m_next);
        EXPECT_EQ(plan.GetFrame().FrameSlotIndex, m_next % m_state.ConfigFrames);
        if (m_state.Suspended) {
            return {.Status = NGraphics::EFrameSubmissionStatus::SUSPENDED, .Completion = {}};
        }
        if (m_state.NotReady > 0) {
            --m_state.NotReady;
            return {.Status = NGraphics::EFrameSubmissionStatus::NOT_READY, .Completion = {}};
        }
        for (const auto& view: world.GetViews()) {
            EXPECT_FLOAT_EQ(view.WorldTransform.Translation.X, static_cast<float>(m_next + 2));
        }
        m_state.Frames.push_back(plan.GetFrame());
        m_state.Views.push_back(world.GetViews().size());
        return {.Status = NGraphics::EFrameSubmissionStatus::PRESENTED,
                .Completion = NGraphics::NBackend::CompletionAccess::Make(1, ++m_next)};
    }
    [[nodiscard]] bool IsCompleted(NGraphics::CompletionPoint) const override {
        EXPECT_EQ(std::this_thread::get_id(), m_state.Owner);
        return m_state.Completed;
    }
    void Stop() noexcept override {
        EXPECT_EQ(std::this_thread::get_id(), m_state.Owner);
        if (m_state.OutputActive) {
            ++m_state.Stops;
            m_state.OutputActive = false;
        }
    }

private:
    State& m_state;
    std::uint64_t m_next = 0;
};

std::unique_ptr<NWindow::NEngine::IWindowEngine> WindowFactory(const NWindow::WindowConfig&) {
    return std::make_unique<TestWindow>(*g_state);
}
std::unique_ptr<NApplication::NRuntime::IGameOutput> OutputFactory(NWindow::Window&,
                                                                   const NGraphics::PresentationConfig& config) {
    g_state->ConfigFrames = config.MaxActiveFrames;
    return std::make_unique<TestOutput>(*g_state);
}

struct ProbeService {
    explicit ProbeService(State& state)
        : Output(state) {
    }
    ~ProbeService() {
        EXPECT_TRUE(Output.WindowLive);
    }
    void Start(const NApplication::ServiceContext& context) {
        EXPECT_EQ(std::this_thread::get_id(), Output.Owner);
        EXPECT_NE(context.Services.TryGet<NScene::SceneManager>(), nullptr);
        if (Output.FailServiceStart) {
            throw std::runtime_error{"service startup"};
        }
        ++Starts;
    }
    void Stop() {
        EXPECT_EQ(std::this_thread::get_id(), Output.Owner);
        EXPECT_FALSE(Output.OutputActive);
        ++Stops;
    }
    State& Output;
    int Starts = 0;
    int Stops = 0;
};

class GameApp final: public NApplication::Application {
public:
    GameApp(const NApplication::ApplicationConfig& config, State& state, bool camera = false)
        : Application(config) {
        Probe = &GetServices().Register<ProbeService, NScene::SceneManager>(state);
        EXPECT_FALSE(GetScenes().GetActiveHandle());
        if (camera) {
            const auto scene = GetScenes().CreateScene("manual");
            Source = GetScenes().Acquire(scene);
            auto& world = Source->GetWorld();
            Camera = world.CreateEntity();
            world.AddComponent<NRenderer::TransformComponent>(Camera);
            world.AddComponent<NRenderer::CameraComponent>(Camera);
            GetScenes().Activate(scene);
            BindCamera(GetMainWindowView(), scene, world.GetEntityReference(Camera));
            world.RegisterSystem({}, [this](NEcs::World&) { ++Systems; }, NEcs::SystemSchedule{.Id = 1});
        }
    }
    ProbeService* Probe = nullptr;
    std::shared_ptr<NScene::Scene> Source;
    NEcs::Entity Camera;
    std::atomic<int> Updates = 0;
    std::atomic<int> Posts = 0;
    std::atomic<int> Systems = 0;
    int Closes = 0;

private:
    void OnClose() override {
        ++Closes;
    }
    void OnUpdate() override {
        EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
        const int update = ++Updates;
        if (Source) {
            Source->GetWorld().GetComponent<NRenderer::TransformComponent>(Camera).Value.Translation.X =
                    static_cast<float>(update);
        }
    }
    void OnPostUpdate() override {
        EXPECT_TRUE(NCommon::TaskSystem::IsCurrentThreadWorker());
        if (Source) {
            Source->GetWorld().GetComponent<NRenderer::TransformComponent>(Camera).Value.Translation.X += 1;
        }
        ++Posts;
    }
};

NApplication::ApplicationConfig Config(std::size_t workers = 1, std::size_t slots = 2) {
    return {.Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
            .MaxActiveFrames = slots,
            .WorkerCount = workers,
            .Presentation = NGraphics::PresentationConfig{.MaxActiveFrames = 99},
            .Game = NApplication::GameConfig{}};
}

class GameApplication: public testing::Test {
protected:
    void SetUp() override {
        g_state = &Output;
        NWindow::NEngine::SetWindowEngineFactoryForTests(&WindowFactory);
        NApplication::NRuntime::SetGameOutputFactoryForTests(&OutputFactory);
    }
    void TearDown() override {
        NApplication::NRuntime::SetGameOutputFactoryForTests(nullptr);
        NWindow::NEngine::SetWindowEngineFactoryForTests(nullptr);
        g_state = nullptr;
    }
    void CloseAfter(GameApp& app, std::size_t frames) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        Output.Events = [this, &app, frames, deadline] {
            if (Output.Frames.size() >= frames) {
                app.RequestShutdown();
            }
            if (std::chrono::steady_clock::now() > deadline) {
                throw std::runtime_error{"game smoke timeout"};
            }
        };
    }
    State Output;
};

TEST_F(GameApplication, EmptyAndInactiveScenesDoNotCreateOrActivateImplicitScene) {
    GameApp app{Config(), Output};
    const auto scene = app.GetScenes().CreateScene("inactive");
    CloseAfter(app, 4);
    app.Run();
    EXPECT_FALSE(app.GetScenes().GetActiveHandle());
    EXPECT_TRUE(app.GetScenes().IsRegistered(scene));
    ASSERT_GE(Output.Views.size(), 4U);
    for (auto views: Output.Views) {
        EXPECT_EQ(views, 0U);
    }
    EXPECT_EQ(app.Probe->Starts, 1);
    EXPECT_EQ(app.Probe->Stops, 1);
    EXPECT_EQ(app.GetServices().GetState(), NApplication::EServiceState::STOPPED);
}

TEST_F(GameApplication, MatrixUsesOneFrameBoundAndJoinsSceneWorkersBeforeOwnerOutput) {
    for (const std::size_t workers: {1U, 2U, 8U}) {
        for (const std::size_t slots: {1U, 2U, 3U}) {
            SCOPED_TRACE(testing::Message() << workers << " workers / " << slots << " slots");
            Output.Close = false;
            Output.Completed = false;
            Output.Frames.clear();
            Output.Views.clear();
            Output.NotReady = 3;
            GameApp app{Config(workers, slots), Output, true};
            int blocked = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            Output.Events = [&] {
                if (Output.Frames.size() == slots && !Output.Completed) {
                    EXPECT_EQ(app.Updates.load(), slots);
                    EXPECT_EQ(app.Posts.load(), slots);
                    if (++blocked == 8) {
                        Output.Completed = true;
                    }
                }
                if (Output.Frames.size() >= slots + 3) {
                    app.RequestShutdown();
                }
                if (std::chrono::steady_clock::now() > deadline) {
                    throw std::runtime_error{"bounded game pipeline timeout"};
                }
            };
            app.Run();
            EXPECT_EQ(Output.ConfigFrames, slots);
            EXPECT_EQ(blocked, 8);
            EXPECT_GE(app.Systems.load(), static_cast<int>(slots + 3));
            for (auto views: Output.Views) {
                EXPECT_EQ(views, 1U);
            }
            const auto diagnostics = app.GetFrameDiagnostics();
            EXPECT_EQ(diagnostics.Submitted, diagnostics.Completed);
            EXPECT_EQ(diagnostics.Completed, diagnostics.Recycled);
            Output.Events = {};
        }
    }
}

TEST_F(GameApplication, ServicesAndPublicReferencesRemainStableAcrossValidRuns) {
    GameApp app{Config(), Output};
    auto* scenes = &app.GetScenes();
    auto* resources = &app.GetResources();
    auto* services = &app.GetServices();
    const auto view = app.GetMainWindowView();
    const auto observed = services->Observe<ProbeService>();
    for (int run = 0; run < 2; ++run) {
        const auto before = app.Updates.load();
        CloseAfter(app, Output.Frames.size() + 3);
        app.Run();
        EXPECT_GT(app.Updates.load(), before);
        EXPECT_EQ(app.Closes, run + 1);
        EXPECT_EQ(scenes, &app.GetScenes());
        EXPECT_EQ(resources, &app.GetResources());
        EXPECT_EQ(services, &app.GetServices());
        EXPECT_EQ(view, app.GetMainWindowView());
        EXPECT_EQ(observed.TryGet(), app.Probe);
        EXPECT_EQ(app.Probe->Starts, run + 1);
        EXPECT_EQ(app.Probe->Stops, run + 1);
        EXPECT_EQ(Output.Starts, run + 1);
        EXPECT_EQ(Output.Stops, run + 1);
    }
}

TEST_F(GameApplication, UnsupportedWindowRestartDoesNotStartServicesOrAdmitFrames) {
    GameApp app{Config(), Output};
    CloseAfter(app, 3);
    app.Run();
    const auto updates = app.Updates.load();
    Output.CanRestart = false;
    NTest::ExpectError(NCommon::EError::UNSUPPORTED, [&] { app.Run(); });
    EXPECT_TRUE(Output.Close);
    EXPECT_EQ(app.Updates.load(), updates);
    EXPECT_EQ(app.Probe->Starts, 1);
    EXPECT_EQ(app.Closes, 1);
    Output.CanRestart = true;
    CloseAfter(app, Output.Frames.size() + 3);
    app.Run();
    EXPECT_GT(app.Updates.load(), updates);
    EXPECT_EQ(app.Probe->Starts, 2);
    EXPECT_EQ(app.Closes, 2);
}

TEST_F(GameApplication, OutputStartupFailureRollsBackServicesAndAllowsRetry) {
    GameApp app{Config(), Output};
    Output.FailStarts = 1;
    EXPECT_THROW(app.Run(), std::runtime_error);
    EXPECT_EQ(app.Updates.load(), 0);
    EXPECT_EQ(app.Probe->Starts, 1);
    EXPECT_EQ(app.Probe->Stops, 1);
    EXPECT_FALSE(Output.OutputActive);
    EXPECT_EQ(app.GetServices().GetState(), NApplication::EServiceState::STOPPED);
    CloseAfter(app, 3);
    app.Run();
    EXPECT_EQ(app.Probe->Starts, 2);
    EXPECT_EQ(app.Probe->Stops, 2);
    NTest::ExpectError(NCommon::EError::INVALID_STATE, [&] { app.GetServices().Register<int>(); });
}

TEST_F(GameApplication, ServiceStartupFailureClosesOutputAndAllowsRetry) {
    GameApp app{Config(), Output};
    Output.FailServiceStart = true;
    EXPECT_THROW(app.Run(), std::runtime_error);
    EXPECT_EQ(app.Updates.load(), 0);
    EXPECT_EQ(Output.Starts, 0);
    EXPECT_FALSE(Output.OutputActive);
    Output.FailServiceStart = false;
    CloseAfter(app, 3);
    app.Run();
    EXPECT_EQ(app.Probe->Starts, 1);
    EXPECT_EQ(app.Probe->Stops, 1);
}

TEST_F(GameApplication, AlreadyClosedWindowDoesNotAdmitFrames) {
    GameApp app{Config(), Output};
    Output.Close = true;
    app.Run();
    EXPECT_EQ(app.Updates.load(), 0);
    EXPECT_TRUE(Output.Frames.empty());
    EXPECT_EQ(app.Probe->Starts, 1);
    EXPECT_EQ(app.Probe->Stops, 1);
}

TEST_F(GameApplication, ZeroExtentRetainsBoundedFramesAndRecoversWithoutDuplicateUpdate) {
    GameApp app{Config(1, 1), Output};
    Output.Size = {};
    int suspended = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    Output.Events = [&] {
        if (Output.Suspended) {
            EXPECT_LE(app.Updates.load(), 1);
            EXPECT_TRUE(Output.Frames.empty());
            if (++suspended == 8) {
                Output.Size = {160, 120};
            }
        }
        if (Output.Frames.size() >= 3) {
            app.RequestShutdown();
        }
        if (std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error{"suspended game timeout"};
        }
    };
    app.Run();
    EXPECT_EQ(suspended, 8);
    EXPECT_EQ(Output.Frames.front().ApplicationFrameIndex, 0U);
    EXPECT_EQ(app.GetFrameDiagnostics().Submitted, app.GetFrameDiagnostics().Recycled);
}

TEST_F(GameApplication, InvalidModeAndFrameBoundFailBeforeCreatingOutput) {
    auto config = Config();
    config.Game->RenderingMode = static_cast<NApplication::ERenderingMode>(99);
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { GameApp app{config, Output}; });
    config = Config();
    config.MaxActiveFrames = 0;
    NTest::ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { GameApp app{config, Output}; });
    EXPECT_EQ(Output.Created, 0);
    EXPECT_EQ(Output.WindowDestroyed, 0);
}

TEST_F(GameApplication, UnavailableOutputRollsBackWindowAndDoesNotPublishCallbacks) {
    NApplication::NRuntime::SetGameOutputFactoryForTests(
            [](NWindow::Window&,
               const NGraphics::PresentationConfig&) -> std::unique_ptr<NApplication::NRuntime::IGameOutput> {
                GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "unavailable test backend");
            });
    NTest::ExpectError(NCommon::EError::UNSUPPORTED, [&] { GameApp app{Config(), Output}; });
    EXPECT_EQ(Output.WindowDestroyed, 1);
    EXPECT_EQ(Output.Created, 0);
}

TEST_F(GameApplication, DisabledModeRetainsLegacyCpuPath) {
    auto config = Config();
    config.Presentation.reset();
    config.Game->RenderingMode = NApplication::ERenderingMode::DISABLED;
    class Legacy final: public NApplication::Application {
    public:
        using Application::Application;
        int Updates = 0;

    private:
        void OnUpdate() override {
            ++Updates;
            RequestShutdown();
        }
    } app{config};
    app.Run();
    EXPECT_EQ(app.Updates, 1);
    EXPECT_EQ(Output.Created, 0);
}

} // namespace
