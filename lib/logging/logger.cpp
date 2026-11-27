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
    if (m_state == ELoggerState::STOPPING) {
        return;
    }
    m_state = ELoggerState::RUNNING;
}

void Logger::Shutdown() noexcept {
    std::shared_ptr<const SinkList> sinks;
    {
        std::unique_lock lock(m_mutex);
        if (m_state == ELoggerState::SHUTDOWN || m_state == ELoggerState::STOPPING) {
            return;
        }
        m_state = ELoggerState::STOPPING;
        m_drained.wait(lock, [this] { return m_activePublications == 0; });
        sinks = std::atomic_load_explicit(&m_sinks, std::memory_order_acquire);
    }
    if (sinks != nullptr) {
        for (const auto& sink: *sinks) {
            try {
                sink->Flush();
            } catch (...) {
                m_sinkFailures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    {
        std::scoped_lock lock(m_mutex);
        m_state = ELoggerState::SHUTDOWN;
    }
}

void Logger::Flush() noexcept {
    const auto sinks = std::atomic_load_explicit(&m_sinks, std::memory_order_acquire);
    if (sinks != nullptr) {
        for (const auto& sink: *sinks) {
            try {
                sink->Flush();
            } catch (...) {
                m_sinkFailures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void Logger::SetConfig(LoggerConfig config) {
    std::scoped_lock lock(m_mutex);
    m_config = config;
}

LoggerConfig Logger::GetConfig() const {
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
    const auto current = std::atomic_load_explicit(&m_sinks, std::memory_order_acquire);
    auto updated = current == nullptr ? std::make_shared<SinkList>() : std::make_shared<SinkList>(*current);
    updated->push_back(std::move(sink));
    std::shared_ptr<const SinkList> published = std::move(updated);
    std::atomic_store_explicit(&m_sinks, std::move(published), std::memory_order_release);
}

void Logger::ClearSinks() noexcept {
    std::shared_ptr<const SinkList> retired;
    {
        std::scoped_lock lock(m_mutex);
        retired = std::atomic_exchange_explicit(&m_sinks, {}, std::memory_order_acq_rel);
    }
}

bool Logger::IsEnabledLocked(ESeverity severity, std::string_view category) const noexcept {
    return m_state == ELoggerState::RUNNING && m_config.Enabled && severity >= m_config.MinimumSeverity &&
           std::ranges::find(m_config.DisabledCategories, category) == m_config.DisabledCategories.end();
}

void Logger::ReleasePublication() noexcept {
    std::scoped_lock lock(m_mutex);
    --m_activePublications;
    if (m_activePublications == 0) {
        m_drained.notify_all();
    }
}

bool Logger::Publish(LogRecord record) noexcept {
    std::shared_ptr<const SinkList> sinks;
    {
        std::scoped_lock lock(m_mutex);
        if (!IsEnabledLocked(record.Severity, record.Category)) {
            return false;
        }
        ++m_activePublications;
        record.Timestamp = std::chrono::system_clock::now();
        record.Sequence = m_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        sinks = std::atomic_load_explicit(&m_sinks, std::memory_order_acquire);
    }
    if (sinks != nullptr) {
        for (const auto& sink: *sinks) {
            try {
                sink->Consume(record);
            } catch (...) {
                m_sinkFailures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    ReleasePublication();
    return true;
}

bool Logger::Log(ESeverity severity,
                 std::string_view category,
                 std::string_view message,
                 std::vector<LogField> fields,
                 std::string_view threadContext,
                 std::string_view taskTraceContext) {
    {
        std::scoped_lock lock(m_mutex);
        if (!IsEnabledLocked(severity, category)) {
            return false;
        }
    }
    LogRecord record{.Severity = severity,
                     .Category = std::string(category),
                     .Message = std::string(message),
                     .Fields = std::move(fields),
                     .ThreadContext = std::string(threadContext),
                     .TaskTraceContext = std::string(taskTraceContext)};
    return Publish(std::move(record));
}

} // namespace NLogging
