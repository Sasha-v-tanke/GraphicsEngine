#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <future>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

    void Update(const NEngine::NRuntime::FrameContext&) override {
        m_updateStarted.set_value();
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Injected update failure");
    }

    void Draw(const NEngine::NRuntime::FrameContext&) override {
        ++m_drawCount;
    }

    [[nodiscard]] int GetDrawCount() const noexcept {
        return m_drawCount;
    }

private:
    std::promise<void>& m_updateStarted;
    int m_drawCount = 0;
};

class ThrowingDrawFrameRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    explicit ThrowingDrawFrameRuntime(std::promise<void>& drawStarted)
        : m_drawStarted(drawStarted) {
    }

    void Update(const NEngine::NRuntime::FrameContext&) override {
        ++m_updateCount;
    }

    void Draw(const NEngine::NRuntime::FrameContext&) override {
        m_drawStarted.set_value();
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Injected draw failure");
    }

    [[nodiscard]] int GetUpdateCount() const noexcept {
        return m_updateCount;
    }

private:
    std::promise<void>& m_drawStarted;
    int m_updateCount = 0;
};

class AllocatingFrameRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    void Update(const NEngine::NRuntime::FrameContext& frame) override {
        std::pmr::vector<int> values{&frame.GetMemoryResource()};
        values.push_back(1);
        values.push_back(2);

        m_updateFrame = frame.GetFrame();
        ++m_updateCount;
    }

    void Draw(const NEngine::NRuntime::FrameContext& frame) override {
        std::pmr::vector<int> values{&frame.GetMemoryResource()};
        values.push_back(3);

        m_drawFrame = frame.GetFrame();
        ++m_drawCount;
    }

    [[nodiscard]] int GetUpdateCount() const noexcept {
        return m_updateCount;
    }

    [[nodiscard]] int GetDrawCount() const noexcept {
        return m_drawCount;
    }

    [[nodiscard]] NEngine::NController::FrameHandle GetUpdateFrame() const noexcept {
        return m_updateFrame;
    }

    [[nodiscard]] NEngine::NController::FrameHandle GetDrawFrame() const noexcept {
        return m_drawFrame;
    }

private:
    int m_updateCount = 0;
    int m_drawCount = 0;
    NEngine::NController::FrameHandle m_updateFrame;
    NEngine::NController::FrameHandle m_drawFrame;
};

class BlockingFrameRuntime final: public NEngine::NRuntime::IFrameRuntime {
public:
    void Update(const NEngine::NRuntime::FrameContext&) override {
        {
            std::lock_guard lock{m_mutex};
            m_updateEntered = true;
        }

        m_condition.notify_all();

        std::unique_lock lock{m_mutex};
        m_condition.wait(lock, [this] { return m_finishUpdate; });
    }

    void Draw(const NEngine::NRuntime::FrameContext&) override {
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
    void Update(const NEngine::NRuntime::FrameContext&) override {
    }

    void Draw(const NEngine::NRuntime::FrameContext& frame) override {
        {
            std::lock_guard lock{m_mutex};
            ++m_drawCount;
        }

        m_condition.notify_all();

        if (frame.GetFrame().GetFrameIndex() == 0) {
            std::unique_lock lock{m_mutex};
            m_condition.wait(lock, [this] { return m_finishFirstDraw; });
        }

        if (frame.GetFrame().GetFrameIndex() == 0) {
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

class FinishFirstDrawGuard final {
public:
    explicit FinishFirstDrawGuard(BlockingFirstDrawRuntime& runtime) noexcept
        : m_runtime(&runtime) {
    }

    ~FinishFirstDrawGuard() {
        if (m_runtime != nullptr) {
            m_runtime->FinishFirstDraw();
        }
    }

    void Release() noexcept {
        m_runtime = nullptr;
    }

private:
    BlockingFirstDrawRuntime* m_runtime = nullptr;
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
    FinishFirstDrawGuard finishFirstDrawGuard{runtimeRef};
    ASSERT_TRUE(runtimeRef.WaitDrawCountAtLeast(1, 2s));

    std::future<void> stopResult = std::async(std::launch::async, [&] { engine->Stop(); });

    EXPECT_TRUE(WaitForState(*engine, NEngine::EEngineState::STOPPING, 2s));
    EXPECT_EQ(stopResult.wait_for(std::chrono::seconds{0}), std::future_status::timeout);

    runtimeRef.FinishFirstDraw();
    finishFirstDrawGuard.Release();
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
    FinishFirstDrawGuard finishFirstDrawGuard{runtimeRef};
    ASSERT_TRUE(runtimeRef.WaitDrawCountAtLeast(1, 2s));

    EXPECT_TRUE(engine->Update());
    EXPECT_TRUE(engine->Draw());

    EXPECT_FALSE(engine->Update());

    runtimeRef.FinishFirstDraw();
    finishFirstDrawGuard.Release();
    EXPECT_TRUE(runtimeRef.WaitFirstDrawFinished(2s));

    EXPECT_TRUE(engine->Update());
    EXPECT_TRUE(engine->Draw());

    engine->Stop();
    EXPECT_FALSE(engine->GetLastError().has_value());
}

TEST(Engine, KeepsCompletedFrameActiveUntilDrawTaskReturns) {
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
    FinishFirstDrawGuard finishFirstDrawGuard{runtimeRef};
    ASSERT_TRUE(runtimeRef.WaitDrawCountAtLeast(1, 2s));

    EXPECT_FALSE(engine->Update());

    runtimeRef.FinishFirstDraw();
    finishFirstDrawGuard.Release();
    EXPECT_TRUE(runtimeRef.WaitFirstDrawFinished(2s));

    EXPECT_TRUE(engine->Update());
    EXPECT_TRUE(engine->Draw());

    engine->Stop();
    EXPECT_FALSE(engine->GetLastError().has_value());
}

TEST(Engine, ProvidesFrameScopedStorageToRuntimeContext) {
    auto runtime = std::make_unique<AllocatingFrameRuntime>();
    AllocatingFrameRuntime& runtimeRef = *runtime;

    std::unique_ptr<NEngine::Engine> engine = NEngine::NRuntime::EngineFactory::Create(
            NEngine::EngineConfig{
                    .MaxActiveFrames = 1,
                    .WorkerCount = 1,
            },
            std::move(runtime));

    engine->Start();

    EXPECT_TRUE(engine->Update());
    EXPECT_TRUE(engine->Draw());

    engine->Stop();

    EXPECT_EQ(runtimeRef.GetUpdateCount(), 1);
    EXPECT_EQ(runtimeRef.GetDrawCount(), 1);
    EXPECT_TRUE(runtimeRef.GetUpdateFrame().IsValid());
    EXPECT_EQ(runtimeRef.GetUpdateFrame(), runtimeRef.GetDrawFrame());
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

TEST(Engine, LatchesDrawRuntimeErrorWithoutCompletingFrame) {
    std::promise<void> drawStarted;
    std::future<void> drawStartedResult = drawStarted.get_future();
    auto runtime = std::make_unique<ThrowingDrawFrameRuntime>(drawStarted);
    ThrowingDrawFrameRuntime& runtimeRef = *runtime;

    std::unique_ptr<NEngine::Engine> engine = NEngine::NRuntime::EngineFactory::Create(
            NEngine::EngineConfig{
                    .MaxActiveFrames = 1,
                    .WorkerCount = 1,
            },
            std::move(runtime));

    engine->Start();

    EXPECT_TRUE(engine->Update());
    EXPECT_TRUE(engine->Draw());

    ASSERT_EQ(drawStartedResult.wait_for(2s), std::future_status::ready);
    drawStartedResult.get();
    EXPECT_TRUE(WaitForState(*engine, NEngine::EEngineState::STOPPING, 2s));

    std::optional<NCommon::ErrorInfo> error = engine->GetLastError();

    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->Code, NCommon::make_error_code(NCommon::EError::INVALID_STATE));
    EXPECT_EQ(error->Message, "Injected draw failure");
    EXPECT_EQ(runtimeRef.GetUpdateCount(), 1);

    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine->Update()); });
    ExpectError(NCommon::EError::INVALID_STATE, [&] { static_cast<void>(engine->Draw()); });

    engine->Stop();
    EXPECT_EQ(engine->GetState(), NEngine::EEngineState::STOPPED);
}

} // namespace
