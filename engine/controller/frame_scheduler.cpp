#include <engine/internal/frame_scheduler.h>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NEngine::NInternal {

FrameScheduler::FrameScheduler(const EngineConfig& config)
    : m_maxActiveFrames(config.MaxActiveFrames) {
    if (m_maxActiveFrames == 0) {
        GRAPHICS_ENGINE_THROW(
                NCommon::EError::INVALID_ARGUMENT,
                "EngineConfig.MaxActiveFrames must be greater than zero");
    }

    m_slots = std::make_unique<FrameExecutionSlot[]>(m_maxActiveFrames);
}

std::optional<FrameHandle> FrameScheduler::TryAcquireFrame() {
    std::lock_guard lock{m_mutex};

    if (m_nextFrameIndex == FrameHandle::INVALID_FRAME_INDEX) {
        GRAPHICS_ENGINE_THROW(
                NCommon::EError::INVALID_STATE,
                "Frame index space is exhausted");
    }

    const std::size_t slotIndex = static_cast<std::size_t>(m_nextFrameIndex % m_maxActiveFrames);

    FrameExecutionSlot& slot = m_slots[slotIndex];

    if (slot.State != EFrameState::FREE) {
        return std::nullopt;
    }

    if (slot.Generation == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(
                NCommon::EError::INVALID_STATE,
                "Frame slot {} generation space is exhausted",
                slotIndex);
    }

    ++slot.Generation;

    slot.FrameIndex = m_nextFrameIndex;
    slot.State = EFrameState::ACQUIRED;

    const FrameHandle frame{
            m_nextFrameIndex,
            slotIndex,
            slot.Generation,
    };

    ++m_nextFrameIndex;

    return frame;
}

void FrameScheduler::ArmFrame(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    TransitionLocked(
            frame,
            slot,
            EFrameState::ACQUIRED,
            EFrameState::WAITING_UPDATE);
}

void FrameScheduler::BeginUpdate(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    TransitionLocked(
            frame,
            slot,
            EFrameState::WAITING_UPDATE,
            EFrameState::UPDATING);
}

void FrameScheduler::EndUpdate(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    TransitionLocked(
            frame,
            slot,
            EFrameState::UPDATING,
            EFrameState::WAITING_DRAW);
}

void FrameScheduler::BeginFinalize(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    TransitionLocked(
            frame,
            slot,
            EFrameState::WAITING_DRAW,
            EFrameState::FINALIZE);
}

void FrameScheduler::CompleteFrame(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    TransitionLocked(
            frame,
            slot,
            EFrameState::FINALIZE,
            EFrameState::COMPLETE);
}

void FrameScheduler::RecycleFrame(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    FrameExecutionSlot& slot = GetSlotLocked(frame);

    if (slot.State != EFrameState::COMPLETE) {
        GRAPHICS_ENGINE_THROW(
                NCommon::EError::INVALID_STATE,
                "Frame {} in slot {} cannot be recycled from state {}",
                frame.GetFrameIndex(),
                frame.GetSlotIndex(),
                static_cast<int>(slot.State));
    }

    slot.Arena.Reset();
    slot.FrameIndex = FrameHandle::INVALID_FRAME_INDEX;
    slot.State = EFrameState::FREE;
}

EFrameState FrameScheduler::GetState(FrameHandle frame) const {
    std::lock_guard lock{m_mutex};

    return GetSlotLocked(frame).State;
}

std::pmr::memory_resource& FrameScheduler::GetMemoryResource(FrameHandle frame) {
    std::lock_guard lock{m_mutex};

    return GetSlotLocked(frame).Arena.GetMemoryResource();
}

FrameScheduler::FrameExecutionSlot& FrameScheduler::GetSlotLocked(FrameHandle frame) {
    return const_cast<FrameExecutionSlot&>(
            static_cast<const FrameScheduler&>(*this).GetSlotLocked(frame));
}

const FrameScheduler::FrameExecutionSlot& FrameScheduler::GetSlotLocked(FrameHandle frame) const {
    if (!frame.IsValid()) {
        GRAPHICS_ENGINE_THROW(
                NCommon::EError::INVALID_ARGUMENT,
                "Invalid frame handle");
    }

    if (frame.GetSlotIndex() >= m_maxActiveFrames) {
        GRAPHICS_ENGINE_THROW(
                NCommon::EError::INVALID_ARGUMENT,
                "Frame slot {} is out of range [0, {})",
                frame.GetSlotIndex(),
                m_maxActiveFrames);
    }

    const FrameExecutionSlot& slot = m_slots[frame.GetSlotIndex()];

    if (slot.FrameIndex != frame.GetFrameIndex() || slot.Generation != frame.GetGeneration()) {
        GRAPHICS_ENGINE_THROW(
                NCommon::EError::INVALID_STATE,
                "Stale frame handle: frame {}, slot {}, generation {}",
                frame.GetFrameIndex(),
                frame.GetSlotIndex(),
                frame.GetGeneration());
    }

    return slot;
}

void FrameScheduler::TransitionLocked(
        FrameHandle frame,
        FrameExecutionSlot& slot,
        EFrameState expectedState,
        EFrameState nextState) {
    if (slot.State != expectedState) {
        GRAPHICS_ENGINE_THROW(
                NCommon::EError::INVALID_STATE,
                "Frame {} in slot {} has state {}, expected {}",
                frame.GetFrameIndex(),
                frame.GetSlotIndex(),
                static_cast<int>(slot.State),
                static_cast<int>(expectedState));
    }

    slot.State = nextState;
}

} // namespace NEngine::NInternal
