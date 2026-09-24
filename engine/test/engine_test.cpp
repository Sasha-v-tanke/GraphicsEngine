#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <engine/engine.h>
#include <engine/runtime/frame_runtime.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <tests/common/test_error.h>

namespace {

using NTest::ExpectError;
using namespace std::chrono_literals;

class ThrowingFrameRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    explicit ThrowingFrameRuntime(std::promise<void>& updateStarted)
        : m_updateStarted(updateStarted) {
    }

    void Update(NEngine::NController::FrameScheduler&, NEngine::NController::FrameHandle) override {
        m_updateStarted.set_value();
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Injected update failure");
    }

    void Draw(NEngine::NController::FrameScheduler&, NEngine::NController::FrameHandle) override {
        ++m_drawCount;
    }

    [[nodiscard]] int GetDrawCount() const noexcept {
        return m_drawCount;
    }

private:
    std::promise<void>& m_updateStarted;
    int m_drawCount = 0;
};

class BlockingFrameRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    void Update(NEngine::NController::FrameScheduler& frameScheduler,
                NEngine::NController::FrameHandle frame) override {
        {
            std::lock_guard lock{m_mutex};
            m_updateEntered = true;
        }

        m_condition.notify_all();

        std::unique_lock lock{m_mutex};
        m_condition.wait(lock, [this] { return m_finishUpdate; });
        lock.unlock();

        frameScheduler.BeginUpdate(frame);
        frameScheduler.EndUpdate(frame);
    }

    void Draw(NEngine::NController::FrameScheduler& frameScheduler, NEngine::NController::FrameHandle frame) override {
        frameScheduler.BeginFinalize(frame);
        frameScheduler.CompleteFrame(frame);
        frameScheduler.RecycleFrame(frame);
    }

    [[nodiscard]] bool WaitUpdateEntered(std::chrono::milliseconds timeout) {
        std::unique_lock lock{m_mutex};
        return m_condition.wait_for(lock, timeout, [this] { return m_updateEntered; });
    }

    void FinishUpdate() {
        {
            std::lock_guard lock{m_mutex};
            m_finishUpdate = true;
        }

        m_condition.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_updateEntered = false;
    bool m_finishUpdate = false;
};

class BlockingFirstDrawRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    void Update(NEngine::NController::FrameScheduler& frameScheduler,
                NEngine::NController::FrameHandle frame) override {
        frameScheduler.BeginUpdate(frame);
        frameScheduler.EndUpdate(frame);
    }

    void Draw(NEngine::NController::FrameScheduler& frameScheduler, NEngine::NController::FrameHandle frame) override {
        {
            std::lock_guard lock{m_mutex};
            ++m_drawCount;
        }

        m_condition.notify_all();

        if (frame.GetFrameIndex() == 0) {
            std::unique_lock lock{m_mutex};
            m_condition.wait(lock, [this] { return m_finishFirstDraw; });
        }

        frameScheduler.BeginFinalize(frame);
        frameScheduler.CompleteFrame(frame);
        frameScheduler.RecycleFrame(frame);

        if (frame.GetFrameIndex() == 0) {
            {
                std::lock_guard lock{m_mutex};
                m_firstDrawFinished = true;
            }

            m_condition.notify_all();
        }
    }

    [[nodiscard]] bool WaitDrawCountAtLeast(int drawCount, std::chrono::milliseconds timeout) {
        std::unique_lock lock{m_mutex};
        return m_condition.wait_for(lock, timeout, [this, drawCount] { return m_drawCount >= drawCount; });
    }

    [[nodiscard]] bool WaitFirstDrawFinished(std::chrono::milliseconds timeout) {
        std::unique_lock lock{m_mutex};
        return m_condition.wait_for(lock, timeout, [this] { return m_firstDrawFinished; });
    }

    void FinishFirstDraw() {
        {
            std::lock_guard lock{m_mutex};
            m_finishFirstDraw = true;
        }

        m_condition.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    int m_drawCount = 0;
    bool m_finishFirstDraw = false;
    bool m_firstDrawFinished = false;
};

class FinishUpdateGuard final {
public:
    explicit FinishUpdateGuard(BlockingFrameRuntime& runtime) noexcept
        : m_runtime(&runtime) {
    }

    ~FinishUpdateGuard() {
        if (m_runtime != nullptr) {
            m_runtime->FinishUpdate();
        }
    }

    void Release() noexcept {
        m_runtime = nullptr;
    }

private:
    BlockingFrameRuntime* m_runtime = nullptr;
};

[[nodiscard]] bool
WaitForState(const NEngine::Engine& engine, NEngine::EEngineState expectedState, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (engine.GetState() != expectedState) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }

        std::this_thread::yield();
    }

    return true;
}

TEST(Engine, FollowsLifecycleStates) {
    NEngine::Engine engine{NEngine::EngineConfig{
            .MaxActiveFrames = 2,
            .WorkerCount = 1,
    }};

    EXPECT_EQ(engine.GetState(), NEngine::EEngineState::CREATED);

    engine.Start();

    EXPECT_EQ(engine.GetState(), NEngine::EEngineState::RUNNING);

    engine.Stop();

    EXPECT_EQ(engine.GetState(), NEngine::EEngineState::STOPPED);
}

TEST(Engine, RejectsRepeatedInvalidCalls) {
    NEngine::Engine engine{NEngine::EngineConfig{
            .MaxActiveFrames = 1,
            .WorkerCount = 1,
    }};

    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine.Update()); });
    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine.Draw()); });

    engine.Start();

    ExpectError(NCommon::EError::INVALID_STATE, [&] { engine.Start(); });
    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine.Draw()); });

    EXPECT_TRUE(engine.Update());
    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine.Update()); });
    EXPECT_TRUE(engine.Draw());
    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine.Draw()); });
}

TEST(Engine, RollsBackPartialStartFailure) {
    NEngine::Engine engine{NEngine::EngineConfig{
            .MaxActiveFrames = 0,
            .WorkerCount = 1,
    }};

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { engine.Start(); });

    EXPECT_EQ(engine.GetState(), NEngine::EEngineState::STOPPED);
    EXPECT_FALSE(engine.GetLastError().has_value());
}

TEST(Engine, StopWaitsForPendingWork) {
    auto runtime = std::make_unique<BlockingFrameRuntime>();
    BlockingFrameRuntime& runtimeRef = *runtime;

    std::unique_ptr<NEngine::Engine> engine = NEngine::NRuntime::EngineFactory::Create(
            NEngine::EngineConfig{
                    .MaxActiveFrames = 64,
                    .WorkerCount = 1,
            },
            std::move(runtime));

    engine->Start();

    EXPECT_TRUE(engine->Update());
    FinishUpdateGuard finishUpdateGuard{runtimeRef};
    ASSERT_TRUE(runtimeRef.WaitUpdateEntered(2s));

    std::future<void> stopResult = std::async(std::launch::async, [&] { engine->Stop(); });

    EXPECT_TRUE(WaitForState(*engine, NEngine::EEngineState::STOPPING, 2s));

    EXPECT_EQ(stopResult.wait_for(std::chrono::seconds{0}), std::future_status::timeout);
    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine->Update()); });
    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine->Draw()); });

    std::promise<void> secondStopStarted;
    std::future<void> secondStopStartedResult = secondStopStarted.get_future();
    std::future<void> secondStopResult = std::async(std::launch::async, [&] {
        secondStopStarted.set_value();
        engine->Stop();
    });

    ASSERT_EQ(secondStopStartedResult.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(secondStopResult.wait_for(std::chrono::seconds{0}), std::future_status::timeout);

    runtimeRef.FinishUpdate();
    finishUpdateGuard.Release();
    ASSERT_EQ(stopResult.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(secondStopResult.wait_for(2s), std::future_status::ready);
    stopResult.get();
    secondStopResult.get();

    EXPECT_EQ(engine->GetState(), NEngine::EEngineState::STOPPED);
    EXPECT_FALSE(engine->GetLastError().has_value());
}

TEST(Engine, StopWaitsForPendingDrawBeforeDestroyingFrameState) {
    auto runtime = std::make_unique<BlockingFirstDrawRuntime>();
    BlockingFirstDrawRuntime& runtimeRef = *runtime;

    std::unique_ptr<NEngine::Engine> engine = NEngine::NRuntime::EngineFactory::Create(
            NEngine::EngineConfig{
                    .MaxActiveFrames = 1,
                    .WorkerCount = 1,
            },
            std::move(runtime));

    engine->Start();

    EXPECT_TRUE(engine->Update());
    EXPECT_TRUE(engine->Draw());
    ASSERT_TRUE(runtimeRef.WaitDrawCountAtLeast(1, 2s));

    std::future<void> stopResult = std::async(std::launch::async, [&] { engine->Stop(); });

    EXPECT_TRUE(WaitForState(*engine, NEngine::EEngineState::STOPPING, 2s));
    EXPECT_EQ(stopResult.wait_for(std::chrono::seconds{0}), std::future_status::timeout);

    runtimeRef.FinishFirstDraw();
    ASSERT_EQ(stopResult.wait_for(2s), std::future_status::ready);
    stopResult.get();

    EXPECT_EQ(engine->GetState(), NEngine::EEngineState::STOPPED);
    EXPECT_TRUE(runtimeRef.WaitFirstDrawFinished(1s));
    EXPECT_FALSE(engine->GetLastError().has_value());
}

TEST(Engine, WraparoundWaitsForNextMappedSlotRecycle) {
    auto runtime = std::make_unique<BlockingFirstDrawRuntime>();
    BlockingFirstDrawRuntime& runtimeRef = *runtime;

    std::unique_ptr<NEngine::Engine> engine = NEngine::NRuntime::EngineFactory::Create(
            NEngine::EngineConfig{
                    .MaxActiveFrames = 2,
                    .WorkerCount = 2,
            },
            std::move(runtime));

    engine->Start();

    EXPECT_TRUE(engine->Update());
    EXPECT_TRUE(engine->Draw());
    ASSERT_TRUE(runtimeRef.WaitDrawCountAtLeast(1, 2s));

    EXPECT_TRUE(engine->Update());
    EXPECT_TRUE(engine->Draw());

    EXPECT_FALSE(engine->Update());

    runtimeRef.FinishFirstDraw();
    EXPECT_TRUE(runtimeRef.WaitFirstDrawFinished(2s));

    EXPECT_TRUE(engine->Update());
    EXPECT_TRUE(engine->Draw());

    engine->Stop();
    EXPECT_FALSE(engine->GetLastError().has_value());
}

TEST(Engine, LatchesRuntimeErrorAndFailsDependentWork) {
    std::promise<void> updateStarted;
    std::future<void> updateStartedResult = updateStarted.get_future();
    auto runtime = std::make_unique<ThrowingFrameRuntime>(updateStarted);
    ThrowingFrameRuntime& runtimeRef = *runtime;

    std::unique_ptr<NEngine::Engine> engine = NEngine::NRuntime::EngineFactory::Create(
            NEngine::EngineConfig{
                    .MaxActiveFrames = 1,
                    .WorkerCount = 1,
            },
            std::move(runtime));

    engine->Start();

    EXPECT_TRUE(engine->Update());
    EXPECT_TRUE(engine->Draw());

    ASSERT_EQ(updateStartedResult.wait_for(2s), std::future_status::ready);
    updateStartedResult.get();
    EXPECT_TRUE(WaitForState(*engine, NEngine::EEngineState::STOPPING, 2s));

    std::optional<NCommon::ErrorInfo> error = engine->GetLastError();

    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->Code, NCommon::make_error_code(NCommon::EError::INVALID_STATE));
    EXPECT_EQ(error->Message, "Injected update failure");
    EXPECT_EQ(runtimeRef.GetDrawCount(), 0);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine->Update()); });
    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine->Draw()); });

    engine->ClearLastError();
    EXPECT_FALSE(engine->GetLastError().has_value());

    engine->Stop();
    EXPECT_EQ(engine->GetState(), NEngine::EEngineState::STOPPED);
}

} // namespace
