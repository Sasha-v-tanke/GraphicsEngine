#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <GraphicsEngine/engine/engine.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NEngine::NRuntime {

// All operations except ObserveGeneration require the same Engine metadata mutex to be held.
// Sharing that lock keeps lifecycle transitions atomic with frame/job admission and owner cleanup.
class EngineStateManager final: private NCommon::NonTransferable {
public:
    using Generation = std::uint64_t;

    [[nodiscard]] Generation ObserveGeneration() const noexcept;
    [[nodiscard]] Generation GetGeneration() const noexcept;
    [[nodiscard]] EEngineState GetState() const noexcept;
    void ValidateStart() const;
    void ValidateNextGeneration() const;
    void AdvanceGeneration() noexcept;
    void MarkRunning() noexcept;
    void EnterStopping() noexcept;
    void RequireRunning(const char* action) const;

    // Returns ownership of cleanup to exactly one Stop caller. Other/stale callers only wait or return.
    [[nodiscard]] bool BeginStop(std::unique_lock<std::mutex>& lock, Generation observed) noexcept;
    void CompleteStop() noexcept;
    void RollbackStart() noexcept;

private:
    std::condition_variable m_stopCondition;
    EEngineState m_state = EEngineState::CREATED;
    bool m_stopInProgress = false;
    Generation m_generation = 0;
    std::atomic<Generation> m_generationSnapshot = 0;
    Generation m_activeStopGeneration = 0;
    Generation m_completedStopGeneration = 0;
};

} // namespace NEngine::NRuntime
