#include "logger.h"

#include <algorithm>
#include <utility>

namespace NLogging {

Logger::Logger(LoggerConfig config)
    : m_config(config) {
}

Logger::~Logger() noexcept {
    Shutdown();
}

void Logger::Start() {
    std::scoped_lock lock(m_mutex);
    m_state = ELoggerState::RUNNING;
}

void Logger::Shutdown() noexcept {
    std::vector<std::shared_ptr<ILogSink>> sinks;
    {
        std::scoped_lock lock(m_mutex);
        if (m_state == ELoggerState::SHUTDOWN) {
            return;
        }
        m_state = ELoggerState::SHUTDOWN;
        sinks = m_sinks;
    }
    for (const auto& sink: sinks) {
        try {
            sink->Flush();
        } catch (...) {
            m_sinkFailures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Logger::Flush() noexcept {
    std::vector<std::shared_ptr<ILogSink>> sinks;
    {
        std::scoped_lock lock(m_mutex);
        sinks = m_sinks;
    }
    for (const auto& sink: sinks) {
        try {
            sink->Flush();
        } catch (...) {
            m_sinkFailures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Logger::SetConfig(LoggerConfig config) noexcept {
    std::scoped_lock lock(m_mutex);
    m_config = config;
}

LoggerConfig Logger::GetConfig() const noexcept {
    std::scoped_lock lock(m_mutex);
    return m_config;
}

ELoggerState Logger::GetState() const noexcept {
    std::scoped_lock lock(m_mutex);
    return m_state;
}

std::uint64_t Logger::GetSinkFailureCount() const noexcept {
    return m_sinkFailures.load(std::memory_order_relaxed);
}

void Logger::AddSink(std::shared_ptr<ILogSink> sink) {
    if (sink == nullptr) {
        return;
    }
    std::scoped_lock lock(m_mutex);
    m_sinks.push_back(std::move(sink));
}

void Logger::ClearSinks() noexcept {
    std::scoped_lock lock(m_mutex);
    m_sinks.clear();
}

bool Logger::IsEnabledLocked(ESeverity severity) const noexcept {
    return m_state == ELoggerState::RUNNING && m_config.Enabled && severity >= m_config.MinimumSeverity;
}

bool Logger::Publish(LogRecord record) noexcept {
    std::vector<std::shared_ptr<ILogSink>> sinks;
    {
        std::scoped_lock lock(m_mutex);
        if (!IsEnabledLocked(record.Severity)) {
            return false;
        }
        record.Timestamp = std::chrono::system_clock::now();
        record.Sequence = m_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        sinks = m_sinks;
    }
    for (const auto& sink: sinks) {
        try {
            sink->Consume(record);
        } catch (...) {
            m_sinkFailures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return true;
}

bool Logger::Log(ESeverity severity,
                 std::string_view category,
                 std::string_view message,
                 std::vector<LogField> fields,
                 std::string_view threadContext,
                 std::string_view taskTraceContext) {
    LogRecord record{.Severity = severity,
                     .Category = std::string(category),
                     .Message = std::string(message),
                     .Fields = std::move(fields),
                     .ThreadContext = std::string(threadContext),
                     .TaskTraceContext = std::string(taskTraceContext)};
    return Publish(std::move(record));
}

} // namespace NLogging
