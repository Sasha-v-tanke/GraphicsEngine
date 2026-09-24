#include "frame_scheduler.h"

#include <atomic>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NEngine::NController {

FrameScheduler::FrameScheduler(const EngineConfig& config)
    : m_ownerId(AcquireOwnerId())
    , m_maxActiveFrames(config.MaxActiveFrames) {
    if (m_maxActiveFrames == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT,
                              "EngineConfig.MaxActiveFrames must be greater than zero");
    }

    m_slots = std::make_unique<FrameExecutionSlot[]>(m_maxActiveFrames);
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

    if (slot.State != EFrameState::FREE) {
        return std::nullopt;
    }

    if (slot.Generation == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Frame slot {} generation space is exhausted", slotIndex);
    }

    ++slot.Generation;

    const Clock::time_point now = Clock::now();

    slot.ApplicationFrameIndex = m_nextApplicationFrameIndex;
    slot.SimulationIndex = m_nextSimulationIndex;
    slot.State = EFrameState::ACQUIRED;
    slot.UpdateSignal = FrameSignal{
            .IsSet = true,
            .Generation = slot.Generation,
            .ApplicationFrameIndex = slot.ApplicationFrameIndex,
            .SimulationIndex = slot.SimulationIndex,
    };
    slot.DrawSignal = {};
    slot.SimulationStartedAt = now;
    slot.DeltaTime =
            m_previousSimulationStartedAt.has_value() ? now - *m_previousSimulationStartedAt : Duration::zero();
    m_previousSimulationStartedAt = now;

    const FrameHandle frame{
            m_ownerId,
            slot.ApplicationFrameIndex,
            slot.SimulationIndex,
            slotIndex,
            slot.Generation,
    };

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

    if (!slot.UpdateSignal.IsSet || slot.UpdateSignal.Generation != frame.GetGeneration()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} has no update signal for generation {}",
                              frame.GetApplicationFrameIndex(),
                              frame.GetFrameSlotIndex(),
                              frame.GetGeneration());
    }

    if (slot.DrawSignal.IsSet) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} already has draw signal for generation {}",
                              frame.GetApplicationFrameIndex(),
                              frame.GetFrameSlotIndex(),
                              frame.GetGeneration());
    }

    slot.DrawSignal = FrameSignal{
            .IsSet = true,
            .Generation = slot.Generation,
            .ApplicationFrameIndex = slot.ApplicationFrameIndex,
            .SimulationIndex = slot.SimulationIndex,
    };
    m_nextCheckpointSignal = ECheckpointSignal::UPDATE;
}

void FrameScheduler::ArmFrame(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    TransitionLocked(frame, slot, EFrameState::ACQUIRED, EFrameState::WAITING_UPDATE);
}

void FrameScheduler::BeginUpdate(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    TransitionLocked(frame, slot, EFrameState::WAITING_UPDATE, EFrameState::UPDATING);
}

void FrameScheduler::EndUpdate(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    TransitionLocked(frame, slot, EFrameState::UPDATING, EFrameState::WAITING_DRAW);
}

void FrameScheduler::BeginFinalize(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    if (!slot.DrawSignal.IsSet || slot.DrawSignal.Generation != frame.GetGeneration()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} has no draw signal for generation {}",
                              frame.GetApplicationFrameIndex(),
                              frame.GetFrameSlotIndex(),
                              frame.GetGeneration());
    }

    TransitionLocked(frame, slot, EFrameState::WAITING_DRAW, EFrameState::FINALIZE);
}

void FrameScheduler::CompleteFrame(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    TransitionLocked(frame, slot, EFrameState::FINALIZE, EFrameState::COMPLETE);
}

void FrameScheduler::RecycleFrame(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    if (slot.State != EFrameState::COMPLETE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} cannot be recycled from state {}",
                              frame.GetFrameIndex(),
                              frame.GetSlotIndex(),
                              static_cast<int>(slot.State));
    }

    slot.Arena.Reset();
    slot.ApplicationFrameIndex = FrameHandle::INVALID_FRAME_INDEX;
    slot.SimulationIndex = FrameHandle::INVALID_FRAME_INDEX;
    slot.UpdateSignal = {};
    slot.DrawSignal = {};
    slot.SimulationStartedAt.reset();
    slot.DeltaTime = Duration::zero();
    slot.State = EFrameState::FREE;
}

void FrameScheduler::AbortFrame(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    if (slot.State == EFrameState::FREE) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} cannot be aborted from state {}",
                              frame.GetFrameIndex(),
                              frame.GetSlotIndex(),
                              static_cast<int>(slot.State));
    }

    if (m_nextCheckpointSignal == ECheckpointSignal::DRAW && !slot.DrawSignal.IsSet &&
        slot.ApplicationFrameIndex + 1 == m_nextApplicationFrameIndex) {
        m_nextCheckpointSignal = ECheckpointSignal::UPDATE;
    }

    slot.Arena.Reset();
    slot.ApplicationFrameIndex = FrameHandle::INVALID_FRAME_INDEX;
    slot.SimulationIndex = FrameHandle::INVALID_FRAME_INDEX;
    slot.UpdateSignal = {};
    slot.DrawSignal = {};
    slot.SimulationStartedAt.reset();
    slot.DeltaTime = Duration::zero();
    slot.State = EFrameState::FREE;
}

EFrameState FrameScheduler::GetState(FrameHandle frame) const {
    std::lock_guard lock{m_mutex};

    return GetSlotLocked(frame).State;
}

FrameScheduler::Duration FrameScheduler::GetDeltaTime(FrameHandle frame) const {
    std::lock_guard lock{m_mutex};

    return GetSlotLocked(frame).DeltaTime;
}

std::pmr::memory_resource& FrameScheduler::GetMemoryResource(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    return GetSlotLocked(frame).Arena.GetMemoryResource();
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

FrameScheduler::FrameExecutionSlot& FrameScheduler::GetSlotLocked(FrameHandle frame) {
    return const_cast<FrameExecutionSlot&>(static_cast<const FrameScheduler&>(*this).GetSlotLocked(frame));
}

const FrameScheduler::FrameExecutionSlot& FrameScheduler::GetSlotLocked(FrameHandle frame) const {
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

    const FrameExecutionSlot& slot = m_slots[frame.GetSlotIndex()];

    if (slot.ApplicationFrameIndex != frame.GetApplicationFrameIndex() ||
        slot.SimulationIndex != frame.GetSimulationIndex() || slot.Generation != frame.GetGeneration()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Stale frame handle: frame {}, slot {}, generation {}",
                              frame.GetApplicationFrameIndex(),
                              frame.GetFrameSlotIndex(),
                              frame.GetGeneration());
    }

    return slot;
}

void FrameScheduler::TransitionLocked(FrameHandle frame,
                                      FrameExecutionSlot& slot,
                                      EFrameState expectedState,
                                      EFrameState nextState) {
    if (slot.State != expectedState) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Frame {} in slot {} has state {}, expected {}",
                              frame.GetFrameIndex(),
                              frame.GetSlotIndex(),
                              static_cast<int>(slot.State),
                              static_cast<int>(expectedState));
    }

    slot.State = nextState;
}

} // namespace NEngine::NController
