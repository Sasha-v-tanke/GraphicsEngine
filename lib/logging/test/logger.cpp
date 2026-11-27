#include <algorithm>
#include <atomic>
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
    Logger logger({.MinimumSeverity = ESeverity::WARNING});
    auto sink = std::make_shared<CollectingSink>();
    logger.AddSink(sink);
    EXPECT_FALSE(logger.Log(ESeverity::FATAL, "core", "before start"));
    logger.Start();
    EXPECT_FALSE(logger.Log(ESeverity::INFO, "core", "filtered"));
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
