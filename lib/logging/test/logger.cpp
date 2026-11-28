#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <GraphicsEngine/lib/logging/logger.h>
#include <gtest/gtest.h>

namespace {
using NLogging::ELoggerState;
using NLogging::ESeverity;
using NLogging::ILogSink;
using NLogging::Logger;
using NLogging::LoggerConfig;
using NLogging::LogRecord;

class CollectingSink final: public ILogSink {
public:
    void Consume(const LogRecord& record) override {
        std::scoped_lock lock(Mutex);
        Records.push_back(record);
    }
    std::mutex Mutex;
    std::vector<LogRecord> Records;
};

class ThrowingSink final: public ILogSink {
public:
    void Consume(const LogRecord&) override {
        throw std::runtime_error("sink failure");
    }
    void Flush() override {
        throw std::runtime_error("flush failure");
    }
};

class ReentrantDestructorSink final: public ILogSink {
public:
    explicit ReentrantDestructorSink(Logger& logger)
        : LoggerRef(logger) {
    }
    ~ReentrantDestructorSink() override {
        static_cast<void>(LoggerRef.GetState());
    }
    void Consume(const LogRecord&) override {
    }

private:
    Logger& LoggerRef;
};

class BlockingSink final: public ILogSink {
public:
    void Consume(const LogRecord&) override {
        std::unique_lock lock(Mutex);
        Entered = true;
        EnteredCv.notify_all();
        ReleaseCv.wait(lock, [this] { return Released; });
    }
    void Release() {
        std::scoped_lock lock(Mutex);
        Released = true;
        ReleaseCv.notify_all();
    }
    void WaitUntilEntered() {
        std::unique_lock lock(Mutex);
        EnteredCv.wait(lock, [this] { return Entered; });
    }
    void Flush() override {
        std::unique_lock lock(Mutex);
        FlushEntered = true;
        FlushEnteredCv.notify_all();
        FlushReleaseCv.wait(lock, [this] { return FlushReleased; });
    }
    void WaitUntilFlushEntered() {
        std::unique_lock lock(Mutex);
        FlushEnteredCv.wait(lock, [this] { return FlushEntered; });
    }
    void ReleaseFlush() {
        std::scoped_lock lock(Mutex);
        FlushReleased = true;
        FlushReleaseCv.notify_all();
    }

private:
    std::mutex Mutex;
    std::condition_variable EnteredCv;
    std::condition_variable ReleaseCv;
    std::condition_variable FlushEnteredCv;
    std::condition_variable FlushReleaseCv;
    bool Entered = false;
    bool Released = false;
    bool FlushEntered = false;
    bool FlushReleased = false;
};

TEST(Logger, OwnsRecordAndSupportsUnicodeFields) {
    Logger logger;
    auto sink = std::make_shared<CollectingSink>();
    logger.AddSink(sink);
    logger.Start();
    std::string message = "Привет, 世界";
    ASSERT_TRUE(logger.Log(ESeverity::WARNING, "renderer.frame", message, {{"frame", "42"}}, "worker-0", "task-12"));
    message.assign("changed");
    std::scoped_lock lock(sink->Mutex);
    ASSERT_EQ(sink->Records.size(), 1U);
    EXPECT_EQ(sink->Records[0].Severity, ESeverity::WARNING);
    EXPECT_EQ(sink->Records[0].Category, "renderer.frame");
    EXPECT_EQ(sink->Records[0].Message, "Привет, 世界");
    EXPECT_EQ(sink->Records[0].Fields[0].Name, "frame");
    EXPECT_EQ(sink->Records[0].ThreadContext, "worker-0");
}

TEST(Logger, EmptyAndThrowingSinksDoNotBreakPublication) {
    Logger logger;
    logger.Start();
    EXPECT_TRUE(logger.Log(ESeverity::INFO, "core", "no sink"));
    logger.AddSink(std::make_shared<ThrowingSink>());
    EXPECT_TRUE(logger.Log(ESeverity::ERROR, "core", "continues"));
    EXPECT_EQ(logger.GetSinkFailureCount(), 1U);
    logger.Shutdown();
    EXPECT_EQ(logger.GetSinkFailureCount(), 2U);
}

TEST(Logger, RuntimeFilteringShutdownAndRestart) {
    Logger logger({.MinimumSeverity = ESeverity::WARNING, .DisabledCategories = {"disabled"}});
    auto sink = std::make_shared<CollectingSink>();
    logger.AddSink(sink);
    EXPECT_FALSE(logger.Log(ESeverity::FATAL, "core", "before start"));
    logger.Start();
    EXPECT_FALSE(logger.Log(ESeverity::INFO, "core", "filtered"));
    EXPECT_FALSE(logger.Log(ESeverity::FATAL, "disabled", "filtered category"));
    EXPECT_TRUE(logger.Log(ESeverity::WARNING, "core", "kept"));
    logger.Shutdown();
    EXPECT_EQ(logger.GetState(), ELoggerState::SHUTDOWN);
    EXPECT_FALSE(logger.Log(ESeverity::FATAL, "core", "after shutdown"));
    logger.Start();
    EXPECT_TRUE(logger.Log(ESeverity::ERROR, "core", "after restart"));
    std::scoped_lock lock(sink->Mutex);
    ASSERT_EQ(sink->Records.size(), 2U);
    EXPECT_LT(sink->Records[0].Sequence, sink->Records[1].Sequence);
}

TEST(Logger, DisabledCategoryRejectsBeforeSinkDelivery) {
    Logger logger({.DisabledCategories = {"disabled"}});
    auto sink = std::make_shared<CollectingSink>();
    logger.AddSink(sink);
    logger.Start();
    EXPECT_FALSE(logger.Log(ESeverity::INFO, "disabled", "ignored"));
    std::scoped_lock lock(sink->Mutex);
    EXPECT_TRUE(sink->Records.empty());
}

TEST(Logger, SinkDestructionDoesNotHoldLoggerMutex) {
    Logger logger;
    logger.AddSink(std::make_shared<ReentrantDestructorSink>(logger));
    logger.ClearSinks();
}

TEST(Logger, ShutdownDrainsAdmittedPublicationBeforeFinalFlush) {
    Logger logger;
    auto sink = std::make_shared<BlockingSink>();
    logger.AddSink(sink);
    logger.Start();
    std::jthread publisher([&] { EXPECT_TRUE(logger.Log(ESeverity::INFO, "core", "in flight")); });
    sink->WaitUntilEntered();
    std::atomic_bool shutdownReturned = false;
    std::jthread shutdown([&] {
        logger.Shutdown();
        shutdownReturned.store(true);
    });
    std::this_thread::yield();
    EXPECT_FALSE(shutdownReturned.load());
    sink->Release();
    sink->WaitUntilFlushEntered();
    EXPECT_FALSE(shutdownReturned.load());
    sink->ReleaseFlush();
    shutdown.join();
    publisher.join();
    EXPECT_TRUE(shutdownReturned.load());
    EXPECT_FALSE(logger.Log(ESeverity::INFO, "core", "after shutdown"));
}

TEST(Logger, ConcurrentShutdownWaitsForFinalFlush) {
    Logger logger;
    auto sink = std::make_shared<BlockingSink>();
    logger.AddSink(sink);
    logger.Start();
    std::jthread publisher([&] { EXPECT_TRUE(logger.Log(ESeverity::INFO, "core", "in flight")); });
    sink->WaitUntilEntered();
    std::atomic_bool firstReturned = false;
    std::atomic_bool secondReturned = false;
    std::jthread first([&] {
        logger.Shutdown();
        firstReturned.store(true);
    });
    std::jthread second([&] {
        logger.Shutdown();
        secondReturned.store(true);
    });
    sink->Release();
    sink->WaitUntilFlushEntered();
    EXPECT_FALSE(firstReturned.load());
    EXPECT_FALSE(secondReturned.load());
    sink->ReleaseFlush();
    first.join();
    second.join();
    publisher.join();
    EXPECT_TRUE(firstReturned.load());
    EXPECT_TRUE(secondReturned.load());
}

TEST(Logger, ConcurrentPublishersKeepUniqueSequences) {
    Logger logger;
    auto sink = std::make_shared<CollectingSink>();
    logger.AddSink(sink);
    logger.Start();
    constexpr int producers = 8;
    constexpr int recordsPerProducer = 1000;
    std::vector<std::jthread> threads;
    for (int producer = 0; producer < producers; ++producer) {
        threads.emplace_back([&] {
            for (int index = 0; index < recordsPerProducer; ++index) {
                ASSERT_TRUE(logger.Log(ESeverity::DEBUG, "concurrent", std::to_string(index)));
            }
        });
    }
    threads.clear();
    std::scoped_lock lock(sink->Mutex);
    ASSERT_EQ(sink->Records.size(), producers * recordsPerProducer);
    std::vector<std::uint64_t> sequences;
    for (const auto& record: sink->Records) {
        sequences.push_back(record.Sequence);
    }
    std::ranges::sort(sequences);
    EXPECT_EQ(std::ranges::adjacent_find(sequences), sequences.end());
}
} // namespace
