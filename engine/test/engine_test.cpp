#include <cstddef>

#include <engine/engine.h>
#include <gtest/gtest.h>
#include <tests/common/test_error.h>

namespace {

using NTest::ExpectError;

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

    EXPECT_TRUE(engine.Update());
    EXPECT_FALSE(engine.Update());
    EXPECT_TRUE(engine.Draw());
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
    NEngine::Engine engine{NEngine::EngineConfig{
            .MaxActiveFrames = 64,
            .WorkerCount = 1,
    }};

    engine.Start();

    for (std::size_t index = 0; index < 64; ++index) {
        EXPECT_TRUE(engine.Update());
    }

    engine.Stop();

    EXPECT_EQ(engine.GetState(), NEngine::EEngineState::STOPPED);
    EXPECT_FALSE(engine.GetLastError().has_value());

    engine.Start();
    EXPECT_TRUE(engine.Update());
    EXPECT_TRUE(engine.Draw());
    engine.Stop();
}

TEST(Engine, KeepsRuntimeErrorChannel) {
    NEngine::Engine engine{NEngine::EngineConfig{
            .MaxActiveFrames = 1,
            .WorkerCount = 1,
    }};

    engine.Start();

    EXPECT_FALSE(engine.GetLastError().has_value());

    engine.ClearLastError();

    EXPECT_FALSE(engine.GetLastError().has_value());

    engine.Stop();
}

} // namespace
