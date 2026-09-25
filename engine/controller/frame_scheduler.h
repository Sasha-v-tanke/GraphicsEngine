#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>

#include <engine/engine_config.h>
#include <lib/common/wrapper/non_transferable.h>

namespace NEngine::NController {

enum class EFrameState {
    FREE,
    ACQUIRED,
    WAITING_UPDATE,
    UPDATING,
    WAITING_DRAW,
    FINALIZE,
    COMPLETE,
};

class FrameHandle final {
public:
    FrameHandle() = default;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_ownerId != 0 && m_generation != 0;
    }

    [[nodiscard]] std::uint64_t GetFrameIndex() const noexcept {
        return m_applicationFrameIndex;
    }

    [[nodiscard]] std::uint64_t GetApplicationFrameIndex() const noexcept {
        return m_applicationFrameIndex;
    }

    [[nodiscard]] std::uint64_t GetSimulationIndex() const noexcept {
        return m_simulationIndex;
    }

    [[nodiscard]] std::size_t GetSlotIndex() const noexcept {
        return m_frameSlotIndex;
    }

    [[nodiscard]] std::size_t GetFrameSlotIndex() const noexcept {
        return m_frameSlotIndex;
    }

    [[nodiscard]] std::uint64_t GetGeneration() const noexcept {
        return m_generation;
    }

    [[nodiscard]] friend bool operator==(FrameHandle lhs, FrameHandle rhs) noexcept = default;

private:
    static constexpr std::uint64_t INVALID_FRAME_INDEX = std::numeric_limits<std::uint64_t>::max();
    static constexpr std::size_t INVALID_SLOT_INDEX = std::numeric_limits<std::size_t>::max();

    FrameHandle(std::uint64_t ownerId,
                std::uint64_t applicationFrameIndex,
                std::uint64_t simulationIndex,
                std::size_t frameSlotIndex,
                std::uint64_t generation) noexcept
        : m_ownerId(ownerId)
        , m_applicationFrameIndex(applicationFrameIndex)
        , m_simulationIndex(simulationIndex)
        , m_frameSlotIndex(frameSlotIndex)
        , m_generation(generation) {
    }

    std::uint64_t m_ownerId = 0;
    std::uint64_t m_applicationFrameIndex = INVALID_FRAME_INDEX;
    std::uint64_t m_simulationIndex = INVALID_FRAME_INDEX;
    std::size_t m_frameSlotIndex = INVALID_SLOT_INDEX;
    std::uint64_t m_generation = 0;

    friend class FrameExecutionSlot;
    friend class FrameScheduler;
    friend class FrameStorage;
};

class FrameScheduler;

class FrameStorage final {
public:
    FrameStorage() = default;

    [[nodiscard]] FrameHandle GetFrame() const noexcept {
        return m_frame;
    }

    [[nodiscard]] std::pmr::memory_resource& GetMemoryResource() const;

private:
    FrameStorage(FrameScheduler& scheduler, FrameHandle frame) noexcept
        : m_scheduler(&scheduler)
        , m_frame(frame) {
    }

private:
    FrameScheduler* m_scheduler = nullptr;
    FrameHandle m_frame;

    friend class FrameScheduler;
};

class FrameExecutionSlot final: private NCommon::NonTransferable {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    void Configure(std::uint64_t ownerId, std::size_t slotIndex) noexcept;

    [[nodiscard]] bool IsFree() const noexcept;

    [[nodiscard]] FrameHandle Acquire(std::uint64_t applicationFrameIndex,
                                      std::uint64_t simulationIndex,
                                      std::optional<Clock::time_point> previousSimulationStartedAt);

    void SignalDraw(FrameHandle frame);

    void Arm(FrameHandle frame);

    void BeginUpdate(FrameHandle frame);

    void EndUpdate(FrameHandle frame);

    void BeginFinalize(FrameHandle frame);

    void Complete(FrameHandle frame);

    void Recycle(FrameHandle frame);

    void Abort(FrameHandle frame);

    [[nodiscard]] bool ShouldReleaseDrawCheckpointOnAbort(FrameHandle frame,
                                                          std::uint64_t nextApplicationFrameIndex) const;

    [[nodiscard]] EFrameState GetState(FrameHandle frame) const;

    [[nodiscard]] Duration GetDeltaTime(FrameHandle frame) const;

    [[nodiscard]] std::pmr::memory_resource& GetMemoryResource(FrameHandle frame);

    [[nodiscard]] Clock::time_point GetSimulationStartedAt(FrameHandle frame) const;

private:
    struct FrameSignal {
        bool IsSet = false;
        std::uint64_t Generation = 0;
        std::uint64_t ApplicationFrameIndex = FrameHandle::INVALID_FRAME_INDEX;
        std::uint64_t SimulationIndex = FrameHandle::INVALID_FRAME_INDEX;
    };

    class FrameArena final: private NCommon::NonTransferable {
    public:
        [[nodiscard]] std::pmr::memory_resource& GetMemoryResource() noexcept {
            return m_resource;
        }

        void Reset() {
            m_resource.release();
        }

    private:
        std::pmr::synchronized_pool_resource m_resource;
    };

    void Validate(FrameHandle frame) const;

    void Transition(FrameHandle frame, EFrameState expectedState, EFrameState nextState);

    void ResetFrameData();

private:
    std::uint64_t m_ownerId = 0;
    std::size_t m_slotIndex = FrameHandle::INVALID_SLOT_INDEX;
    EFrameState m_state = EFrameState::FREE;
    std::uint64_t m_applicationFrameIndex = FrameHandle::INVALID_FRAME_INDEX;
    std::uint64_t m_simulationIndex = FrameHandle::INVALID_FRAME_INDEX;
    std::uint64_t m_generation = 0;
    FrameSignal m_updateSignal;
    FrameSignal m_drawSignal;
    std::optional<Clock::time_point> m_simulationStartedAt;
    Duration m_deltaTime = Duration::zero();
    FrameArena m_arena;
};

class FrameScheduler final: private NCommon::NonTransferable {
public:
    using Clock = FrameExecutionSlot::Clock;
    using Duration = FrameExecutionSlot::Duration;

    explicit FrameScheduler(const EngineConfig& config);

    [[nodiscard]] std::optional<FrameHandle> TryAcquireFrame();

    void SignalDraw(FrameHandle frame);

    void ArmFrame(FrameHandle frame);

    void BeginUpdate(FrameHandle frame);

    void EndUpdate(FrameHandle frame);

    void BeginFinalize(FrameHandle frame);

    void CompleteFrame(FrameHandle frame);

    void RecycleFrame(FrameHandle frame);

    void AbortFrame(FrameHandle frame);

    [[nodiscard]] EFrameState GetState(FrameHandle frame) const;

    [[nodiscard]] Duration GetDeltaTime(FrameHandle frame) const;

    [[nodiscard]] FrameStorage GetFrameStorage(FrameHandle frame);

    [[nodiscard]] std::size_t GetMaxActiveFrames() const noexcept {
        return m_maxActiveFrames;
    }

private:
    enum class ECheckpointSignal {
        UPDATE,
        DRAW,
    };

    [[nodiscard]] static std::uint64_t AcquireOwnerId();

    [[nodiscard]] FrameExecutionSlot& GetSlotLocked(FrameHandle frame);

    [[nodiscard]] const FrameExecutionSlot& GetSlotLocked(FrameHandle frame) const;

    [[nodiscard]] std::pmr::memory_resource& GetMemoryResource(FrameHandle frame);

private:
    mutable std::mutex m_mutex;

    const std::uint64_t m_ownerId;
    const std::size_t m_maxActiveFrames;

    std::unique_ptr<FrameExecutionSlot[]> m_slots;

    ECheckpointSignal m_nextCheckpointSignal = ECheckpointSignal::UPDATE;
    std::uint64_t m_nextApplicationFrameIndex = 0;
    std::uint64_t m_nextSimulationIndex = 0;
    std::optional<Clock::time_point> m_previousSimulationStartedAt;

    friend class FrameStorage;
};

} // namespace NEngine::NController
