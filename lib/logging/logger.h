#pragma once

#include <atomic>
#include <chrono>
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
    std::chrono::system_clock::time_point Timestamp;
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
    SHUTDOWN,
};

struct LoggerConfig final {
    ESeverity MinimumSeverity = ESeverity::TRACE;
    bool Enabled = true;
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

    void SetConfig(LoggerConfig config) noexcept;
    [[nodiscard]] LoggerConfig GetConfig() const noexcept;
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
    [[nodiscard]] bool IsEnabledLocked(ESeverity severity) const noexcept;

    mutable std::mutex m_mutex;
    LoggerConfig m_config;
    ELoggerState m_state = ELoggerState::CREATED;
    std::vector<std::shared_ptr<ILogSink>> m_sinks;
    std::atomic_uint64_t m_sequence = 0;
    std::atomic_uint64_t m_sinkFailures = 0;
};

} // namespace NLogging
