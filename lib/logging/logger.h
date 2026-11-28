#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace NLogging {

enum class ESeverity : std::uint8_t {
    TRACE,
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL,
};

struct LogField final {
    std::string Name;
    std::string Value;
};

// Values are owned by the record and remain valid for the whole sink invocation.
struct LogRecord final {
    std::chrono::system_clock::time_point Timestamp{};
    std::uint64_t Sequence = 0;
    ESeverity Severity = ESeverity::INFO;
    std::string Category;
    std::string Message;
    std::vector<LogField> Fields;
    std::string ThreadContext;
    std::string TaskTraceContext;
};

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void Consume(const LogRecord& record) = 0;
    virtual void Flush() {
    }
};

enum class ELoggerState : std::uint8_t {
    CREATED,
    RUNNING,
    STOPPING,
    SHUTDOWN,
};

struct LoggerConfig final {
    ESeverity MinimumSeverity = ESeverity::TRACE;
    bool Enabled = true;
    std::vector<std::string> DisabledCategories;
};

// Synchronous, application-owned logger. It has no global instance and never invokes a sink while holding its mutex.
class Logger final {
public:
    explicit Logger(LoggerConfig config = {});
    ~Logger() noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void Start();
    void Shutdown() noexcept;
    void Flush() noexcept;

    void SetConfig(LoggerConfig config);
    [[nodiscard]] LoggerConfig GetConfig() const;
    [[nodiscard]] ELoggerState GetState() const noexcept;
    [[nodiscard]] std::uint64_t GetSinkFailureCount() const noexcept;

    // The shared ownership contract lets an in-flight snapshot safely outlive sink removal.
    void AddSink(std::shared_ptr<ILogSink> sink);
    void ClearSinks() noexcept;

    // Returns false when logging is disabled, below the runtime threshold, or not running.
    bool Publish(LogRecord record) noexcept;
    bool Log(ESeverity severity,
             std::string_view category,
             std::string_view message,
             std::vector<LogField> fields = {},
             std::string_view threadContext = {},
             std::string_view taskTraceContext = {});

private:
    using SinkList = std::vector<std::shared_ptr<ILogSink>>;

    [[nodiscard]] bool IsEnabledLocked(ESeverity severity, std::string_view category) const noexcept;
    void ReleasePublication() noexcept;

    mutable std::mutex m_mutex;
    std::condition_variable m_drained;
    std::condition_variable m_shutdownComplete;
    LoggerConfig m_config;
    ELoggerState m_state = ELoggerState::CREATED;
    std::uint64_t m_shutdownGeneration = 0;
    std::uint64_t m_completedShutdownGeneration = 0;
    std::size_t m_activePublications = 0;
    std::shared_ptr<const SinkList> m_sinks;
    std::atomic_uint64_t m_sequence = 0;
    std::atomic_uint64_t m_sinkFailures = 0;
};

} // namespace NLogging

#ifndef GRAPHICS_ENGINE_LOG_COMPILED_MINIMUM_SEVERITY
    #define GRAPHICS_ENGINE_LOG_COMPILED_MINIMUM_SEVERITY 0
#endif

// The category/message expressions are not evaluated when the selected severity is compiled out.
#define GRAPHICS_ENGINE_LOG(logger, severity, category, message, ...)                                                  \
    do {                                                                                                               \
        if constexpr (static_cast<unsigned>(severity) >= GRAPHICS_ENGINE_LOG_COMPILED_MINIMUM_SEVERITY) {              \
            static_cast<void>((logger).Log((severity), (category), (message)__VA_OPT__(, ) __VA_ARGS__));              \
        }                                                                                                              \
    } while (false)
