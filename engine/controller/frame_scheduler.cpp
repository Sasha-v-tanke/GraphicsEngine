#include "frame_scheduler.h"

#include <atomic>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NEngine::NController {

std::pmr::memory_resource& FrameStorage::GetMemoryResource() const {
    if (m_scheduler == nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid frame storage");
    }

    return m_scheduler->GetMemoryResource(m_frame);
}

void FrameExecutionSlot::Configure(std::uint64_t ownerId, std::size_t slotIndex) noexcept {
    m_ownerId = ownerId;
    m_slotIndex = slotIndex;
}

bool FrameExecutionSlot::IsFree() const noexcept {
    return m_state == EFrameState::FREE;
}

FrameHandle FrameExecutionSlot::Acquire(std::uint64_t applicationFrameIndex,
                                        std::uint64_t simulationIndex,
                                        std::optional<Clock::time_point> previousSimulationStartedAt) {
    if (!IsFree()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame slot {} cannot be acquired from state {}",
                              m_slotIndex,
                              static_cast<int>(m_state));
    }

    if (m_generation == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame slot {} generation space is exhausted",
                              m_slotIndex);
    }

    ++m_generation;

    const Clock::time_point now = Clock::now();

    m_applicationFrameIndex = applicationFrameIndex;
    m_simulationIndex = simulationIndex;
    m_state = EFrameState::ACQUIRED;
    m_updateSignal = FrameSignal{
            .IsSet = true,
            .Generation = m_generation,
            .ApplicationFrameIndex = m_applicationFrameIndex,
            .SimulationIndex = m_simulationIndex,
    };
    m_drawSignal = {};
    m_simulationStartedAt = now;
    m_deltaTime = previousSimulationStartedAt.has_value() ? now - *previousSimulationStartedAt : Duration::zero();

    return FrameHandle{
            m_ownerId,
            m_applicationFrameIndex,
            m_simulationIndex,
            m_slotIndex,
            m_generation,
    };
}

void FrameExecutionSlot::SignalDraw(FrameHandle frame) {
    Validate(frame);

    if (!m_updateSignal.IsSet || m_updateSignal.Generation != frame.GetGeneration()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} has no update signal for generation {}",
                              frame.GetApplicationFrameIndex(),
                              frame.GetFrameSlotIndex(),
                              frame.GetGeneration());
    }

    if (m_drawSignal.IsSet) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} already has draw signal for generation {}",
                              frame.GetApplicationFrameIndex(),
                              frame.GetFrameSlotIndex(),
                              frame.GetGeneration());
    }

    m_drawSignal = FrameSignal{
            .IsSet = true,
            .Generation = m_generation,
            .ApplicationFrameIndex = m_applicationFrameIndex,
            .SimulationIndex = m_simulationIndex,
    };
}

void FrameExecutionSlot::Arm(FrameHandle frame) {
    Transition(frame, EFrameState::ACQUIRED, EFrameState::WAITING_UPDATE);
}

void FrameExecutionSlot::BeginUpdate(FrameHandle frame) {
    Transition(frame, EFrameState::WAITING_UPDATE, EFrameState::UPDATING);
}

void FrameExecutionSlot::EndUpdate(FrameHandle frame) {
    Transition(frame, EFrameState::UPDATING, EFrameState::WAITING_DRAW);
}

void FrameExecutionSlot::BeginFinalize(FrameHandle frame) {
    Validate(frame);

    if (!m_drawSignal.IsSet || m_drawSignal.Generation != frame.GetGeneration()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} has no draw signal for generation {}",
                              frame.GetApplicationFrameIndex(),
                              frame.GetFrameSlotIndex(),
                              frame.GetGeneration());
    }

    Transition(frame, EFrameState::WAITING_DRAW, EFrameState::FINALIZE);
}

void FrameExecutionSlot::Complete(FrameHandle frame) {
    Transition(frame, EFrameState::FINALIZE, EFrameState::COMPLETE);
}

void FrameExecutionSlot::Recycle(FrameHandle frame) {
    Validate(frame);

    if (m_state != EFrameState::COMPLETE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} cannot be recycled from state {}",
                              frame.GetFrameIndex(),
                              frame.GetSlotIndex(),
                              static_cast<int>(m_state));
    }

    ResetFrameData();
}

void FrameExecutionSlot::Abort(FrameHandle frame) {
    Validate(frame);

    if (m_state == EFrameState::FREE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} cannot be aborted from state {}",
                              frame.GetFrameIndex(),
                              frame.GetSlotIndex(),
                              static_cast<int>(m_state));
    }

    ResetFrameData();
}

bool FrameExecutionSlot::ShouldReleaseDrawCheckpointOnAbort(FrameHandle frame,
                                                            std::uint64_t nextApplicationFrameIndex) const {
    Validate(frame);

    return !m_drawSignal.IsSet && m_applicationFrameIndex + 1 == nextApplicationFrameIndex;
}

EFrameState FrameExecutionSlot::GetState(FrameHandle frame) const {
    Validate(frame);

    return m_state;
}

FrameExecutionSlot::Duration FrameExecutionSlot::GetDeltaTime(FrameHandle frame) const {
    Validate(frame);

    return m_deltaTime;
}

std::pmr::memory_resource& FrameExecutionSlot::GetMemoryResource(FrameHandle frame) {
    Validate(frame);

    return m_arena.GetMemoryResource();
}

FrameExecutionSlot::Clock::time_point FrameExecutionSlot::GetSimulationStartedAt(FrameHandle frame) const {
    Validate(frame);

    return *m_simulationStartedAt;
}

void FrameExecutionSlot::Validate(FrameHandle frame) const {
    if (!frame.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid frame handle");
    }

    if (frame.m_ownerId != m_ownerId) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Frame handle belongs to another FrameScheduler");
    }

    if (frame.GetSlotIndex() != m_slotIndex) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Frame slot {} does not match execution slot {}",
                              frame.GetSlotIndex(),
                              m_slotIndex);
    }

    if (m_applicationFrameIndex != frame.GetApplicationFrameIndex() ||
        m_simulationIndex != frame.GetSimulationIndex() || m_generation != frame.GetGeneration()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Stale frame handle: frame {}, slot {}, generation {}",
                              frame.GetApplicationFrameIndex(),
                              frame.GetFrameSlotIndex(),
                              frame.GetGeneration());
    }
}

void FrameExecutionSlot::Transition(FrameHandle frame, EFrameState expectedState, EFrameState nextState) {
    Validate(frame);

    if (m_state != expectedState) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} has state {}, expected {}",
                              frame.GetFrameIndex(),
                              frame.GetSlotIndex(),
                              static_cast<int>(m_state),
                              static_cast<int>(expectedState));
    }

    m_state = nextState;
}

void FrameExecutionSlot::ResetFrameData() {
    m_arena.Reset();
    m_applicationFrameIndex = FrameHandle::INVALID_FRAME_INDEX;
    m_simulationIndex = FrameHandle::INVALID_FRAME_INDEX;
    m_updateSignal = {};
    m_drawSignal = {};
    m_simulationStartedAt.reset();
    m_deltaTime = Duration::zero();
    m_state = EFrameState::FREE;
}

FrameScheduler::FrameScheduler(const EngineConfig& config)
    : m_ownerId(AcquireOwnerId())
    , m_maxActiveFrames(config.MaxActiveFrames) {
    if (m_maxActiveFrames == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "EngineConfig.MaxActiveFrames must be greater than zero");
    }

    m_slots = std::make_unique<FrameExecutionSlot[]>(m_maxActiveFrames);

    for (std::size_t slotIndex = 0; slotIndex < m_maxActiveFrames; ++slotIndex) {
        m_slots[slotIndex].Configure(m_ownerId, slotIndex);
    }
}

std::optional<FrameHandle> FrameScheduler::TryAcquireFrame() {
    std::lock_guard lock{m_mutex};

    if (m_nextCheckpointSignal != ECheckpointSignal::UPDATE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application update checkpoint is out of order");
    }

    if (m_nextApplicationFrameIndex == FrameHandle::INVALID_FRAME_INDEX) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application frame index space is exhausted");
    }

    if (m_nextSimulationIndex == FrameHandle::INVALID_FRAME_INDEX) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Simulation index space is exhausted");
    }

    const std::size_t slotIndex = static_cast<std::size_t>(m_nextApplicationFrameIndex % m_maxActiveFrames);

    FrameExecutionSlot& slot = m_slots[slotIndex];

    if (!slot.IsFree()) {
        return std::nullopt;
    }

    const FrameHandle frame =
            slot.Acquire(m_nextApplicationFrameIndex, m_nextSimulationIndex, m_previousSimulationStartedAt);
    m_previousSimulationStartedAt = slot.GetSimulationStartedAt(frame);

    ++m_nextApplicationFrameIndex;
    ++m_nextSimulationIndex;
    m_nextCheckpointSignal = ECheckpointSignal::DRAW;

    return frame;
}

void FrameScheduler::SignalDraw(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    if (m_nextCheckpointSignal != ECheckpointSignal::DRAW) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Application draw checkpoint is out of order");
    }

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    slot.SignalDraw(frame);
    m_nextCheckpointSignal = ECheckpointSignal::UPDATE;
}

void FrameScheduler::ArmFrame(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    slot.Arm(frame);
}

void FrameScheduler::BeginUpdate(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    slot.BeginUpdate(frame);
}

void FrameScheduler::EndUpdate(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    slot.EndUpdate(frame);
}

void FrameScheduler::BeginFinalize(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    slot.BeginFinalize(frame);
}

void FrameScheduler::CompleteFrame(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    slot.Complete(frame);
}

void FrameScheduler::RecycleFrame(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    slot.Recycle(frame);
}

void FrameScheduler::AbortFrame(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    if (m_nextCheckpointSignal == ECheckpointSignal::DRAW &&
        slot.ShouldReleaseDrawCheckpointOnAbort(frame, m_nextApplicationFrameIndex)) {
        m_nextCheckpointSignal = ECheckpointSignal::UPDATE;
    }

    slot.Abort(frame);
}

EFrameState FrameScheduler::GetState(FrameHandle frame) const {
    std::lock_guard lock{m_mutex};

    return GetSlotLocked(frame).GetState(frame);
}

FrameScheduler::Duration FrameScheduler::GetDeltaTime(FrameHandle frame) const {
    std::lock_guard lock{m_mutex};

    return GetSlotLocked(frame).GetDeltaTime(frame);
}

FrameStorage FrameScheduler::GetFrameStorage(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    static_cast<void>(GetSlotLocked(frame).GetState(frame));

    return FrameStorage{*this, frame};
}

std::pmr::memory_resource& FrameScheduler::GetMemoryResource(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    return GetSlotLocked(frame).GetMemoryResource(frame);
}

std::uint64_t FrameScheduler::AcquireOwnerId() {
    static std::atomic<std::uint64_t> nextOwnerId{1};

    std::uint64_t current = nextOwnerId.load(std::memory_order_relaxed);

    while (true) {
        if (current == std::numeric_limits<std::uint64_t>::max()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "FrameScheduler identity space is exhausted");
        }

        if (nextOwnerId.compare_exchange_weak(current,
                                              current + 1,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
            return current;
        }
    }
}

FrameExecutionSlot& FrameScheduler::GetSlotLocked(FrameHandle frame) {
    return const_cast<FrameExecutionSlot&>(static_cast<const FrameScheduler&>(*this).GetSlotLocked(frame));
}

const FrameExecutionSlot& FrameScheduler::GetSlotLocked(FrameHandle frame) const {
    if (!frame.IsValid()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid frame handle");
    }

    if (frame.m_ownerId != m_ownerId) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Frame handle belongs to another FrameScheduler");
    }

    if (frame.GetSlotIndex() >= m_maxActiveFrames) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "Frame slot {} is out of range [0, {})",
                              frame.GetSlotIndex(),
                              m_maxActiveFrames);
    }

    return m_slots[frame.GetSlotIndex()];
}

} // namespace NEngine::NController
