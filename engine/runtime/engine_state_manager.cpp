#include "engine_state_manager.h"

#include <limits>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NEngine::NRuntime {

EngineStateManager::Generation EngineStateManager::ObserveGeneration() const noexcept {
    return m_generationSnapshot.load(std::memory_order_acquire);
}

EngineStateManager::Generation EngineStateManager::GetGeneration() const noexcept {
    return m_generation;
}

EEngineState EngineStateManager::GetState() const noexcept {
    return m_state;
}

void EngineStateManager::ValidateStart() const {
    if (m_state != EEngineState::CREATED && m_state != EEngineState::STOPPED) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Engine cannot start from state {}",
                              static_cast<int>(m_state));
    }
}

void EngineStateManager::ValidateNextGeneration() const {
    if (m_generation == std::numeric_limits<Generation>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Engine run generation space exhausted");
    }
}

void EngineStateManager::AdvanceGeneration() noexcept {
    ++m_generation;
    m_generationSnapshot.store(m_generation, std::memory_order_release);
}

void EngineStateManager::MarkRunning() noexcept {
    m_state = EEngineState::RUNNING;
}

void EngineStateManager::EnterStopping() noexcept {
    m_state = EEngineState::STOPPING;
}

void EngineStateManager::RequireRunning(const char* action) const {
    if (m_state != EEngineState::RUNNING) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Engine cannot {} from state {}",
                              action,
                              static_cast<int>(m_state));
    }
}

bool EngineStateManager::BeginStop(std::unique_lock<std::mutex>& lock, Generation observed) noexcept {
    if (m_state == EEngineState::CREATED || m_state == EEngineState::STOPPED) {
        m_state = EEngineState::STOPPED;
        m_stopCondition.notify_all();
        return false;
    }
    if (m_stopInProgress) {
        m_stopCondition.wait(lock,
                             [this, observed] { return !m_stopInProgress || m_completedStopGeneration >= observed; });
        return false;
    }
    if (m_generation != observed) {
        return false;
    }
    m_stopInProgress = true;
    m_activeStopGeneration = observed;
    EnterStopping();
    return true;
}

void EngineStateManager::CompleteStop() noexcept {
    m_state = EEngineState::STOPPED;
    m_stopInProgress = false;
    m_completedStopGeneration = m_activeStopGeneration;
    m_stopCondition.notify_all();
}

void EngineStateManager::RollbackStart() noexcept {
    m_state = EEngineState::STOPPED;
    m_stopInProgress = false;
    m_activeStopGeneration = m_generation;
    m_completedStopGeneration = m_generation;
    m_stopCondition.notify_all();
}

} // namespace NEngine::NRuntime
