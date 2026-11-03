#include "system_run.h"

#include <utility>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NEcs::NInternal {

bool SystemRun::IsTerminal() const noexcept {
    return m_state && m_state->TryGetOutcome().has_value();
}
std::optional<SystemRunOutcome> SystemRun::TryGetOutcome() const {
    return m_state ? m_state->TryGetOutcome() : std::nullopt;
}
void SystemRun::RequestCancel() const noexcept {
    if (m_state) {
        m_state->RequestCancel();
    }
}
PreparedSystemRun::PreparedSystemRun(std::shared_ptr<SystemRunControl> state)
    : m_state(std::move(state)) {
}
PreparedSystemRun::PreparedSystemRun(PreparedSystemRun&&) noexcept = default;
PreparedSystemRun& PreparedSystemRun::operator=(PreparedSystemRun&& other) noexcept {
    if (this != &other) {
        if (m_state) {
            m_state->Abandon();
        }
        m_state = std::move(other.m_state);
    }
    return *this;
}
PreparedSystemRun::~PreparedSystemRun() {
    if (m_state) {
        m_state->Abandon();
    }
}
SystemRun PreparedSystemRun::GetHandle() const {
    return SystemRun{m_state};
}
void PreparedSystemRun::Start() {
    if (!m_state) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Empty prepared ECS run");
    }
    m_state->Start();
}
PreparedSystemRun PrepareSystems(std::shared_ptr<World> world,
                                 NCommon::TaskSystem& tasks,
                                 std::function<void(SystemRunOutcome)> onFinished) {
    return WorldExecutionAccess::Prepare(std::move(world), tasks, std::move(onFinished));
}
} // namespace NEcs::NInternal
