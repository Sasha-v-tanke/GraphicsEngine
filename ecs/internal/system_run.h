#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>

namespace NCommon {
class TaskSystem;
}
namespace NEcs {
class World;
}

namespace NEcs::NInternal {

enum class ESystemRunStatus {
    PREPARED,
    PUBLISHING,
    RUNNING,
    FINALIZING,
    COMPLETED,
    FAILED,
    CANCELLED,
};

struct SystemRunOutcome {
    ESystemRunStatus Status = ESystemRunStatus::CANCELLED;
    std::exception_ptr Error = {};
    std::optional<std::uint64_t> FailedSystem = {};
};

class SystemRunControl {
public:
    virtual ~SystemRunControl() = default;
    virtual std::optional<SystemRunOutcome> TryGetOutcome() const = 0;
    virtual void RequestCancel() noexcept = 0;
    virtual void Start() = 0;
    virtual void Abandon() noexcept = 0;
};

// Observation never cancels a run; completion includes producer payload retirement.
class SystemRun {
public:
    SystemRun() = default;
    explicit SystemRun(std::shared_ptr<SystemRunControl> state)
        : m_state(std::move(state)) {
    }
    [[nodiscard]] bool IsTerminal() const noexcept;
    [[nodiscard]] std::optional<SystemRunOutcome> TryGetOutcome() const;
    void RequestCancel() const noexcept;

private:
    std::shared_ptr<SystemRunControl> m_state;
};

// Publication owner. Install cancellation before Start, which may complete inline.
class PreparedSystemRun {
public:
    explicit PreparedSystemRun(std::shared_ptr<SystemRunControl> state);
    PreparedSystemRun(PreparedSystemRun&&) noexcept;
    PreparedSystemRun& operator=(PreparedSystemRun&&) noexcept;
    PreparedSystemRun(const PreparedSystemRun&) = delete;
    PreparedSystemRun& operator=(const PreparedSystemRun&) = delete;
    ~PreparedSystemRun();
    [[nodiscard]] SystemRun GetHandle() const;
    void Start();

private:
    std::shared_ptr<SystemRunControl> m_state;
};

class WorldExecutionAccess {
public:
    static PreparedSystemRun
    Prepare(std::shared_ptr<World> world, NCommon::TaskSystem& tasks, std::function<void(SystemRunOutcome)> onFinished);
};

[[nodiscard]] PreparedSystemRun PrepareSystems(std::shared_ptr<World> world,
                                               NCommon::TaskSystem& tasks,
                                               std::function<void(SystemRunOutcome)> onFinished);

} // namespace NEcs::NInternal
