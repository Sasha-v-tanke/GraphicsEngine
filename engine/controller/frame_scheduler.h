#pragma once

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
        return m_frameIndex;
    }

    [[nodiscard]] std::size_t GetSlotIndex() const noexcept {
        return m_slotIndex;
    }

    [[nodiscard]] std::uint64_t GetGeneration() const noexcept {
        return m_generation;
    }

    [[nodiscard]] friend bool operator==(FrameHandle lhs, FrameHandle rhs) noexcept = default;

private:
    static constexpr std::uint64_t INVALID_FRAME_INDEX = std::numeric_limits<std::uint64_t>::max();
    static constexpr std::size_t INVALID_SLOT_INDEX = std::numeric_limits<std::size_t>::max();

    FrameHandle(std::uint64_t ownerId,
                std::uint64_t frameIndex,
                std::size_t slotIndex,
                std::uint64_t generation) noexcept
        : m_ownerId(ownerId)
        , m_frameIndex(frameIndex)
        , m_slotIndex(slotIndex)
        , m_generation(generation) {
    }

    std::uint64_t m_ownerId = 0;
    std::uint64_t m_frameIndex = INVALID_FRAME_INDEX;
    std::size_t m_slotIndex = INVALID_SLOT_INDEX;
    std::uint64_t m_generation = 0;

    friend class FrameScheduler;
};

class FrameScheduler final: private NCommon::NonTransferable {
public:
    explicit FrameScheduler(const EngineConfig& config);

    [[nodiscard]] std::optional<FrameHandle> TryAcquireFrame();

    void ArmFrame(FrameHandle frame);

    void BeginUpdate(FrameHandle frame);

    void EndUpdate(FrameHandle frame);

    void BeginFinalize(FrameHandle frame);

    void CompleteFrame(FrameHandle frame);

    void RecycleFrame(FrameHandle frame);

    [[nodiscard]] EFrameState GetState(FrameHandle frame) const;

    [[nodiscard]] std::pmr::memory_resource& GetMemoryResource(FrameHandle frame);

    [[nodiscard]] std::size_t GetMaxActiveFrames() const noexcept {
        return m_maxActiveFrames;
    }

private:
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

    struct FrameExecutionSlot {
        EFrameState State = EFrameState::FREE;
        std::uint64_t FrameIndex = FrameHandle::INVALID_FRAME_INDEX;
        std::uint64_t Generation = 0;
        FrameArena Arena;
    };

    [[nodiscard]] static std::uint64_t AcquireOwnerId();

    [[nodiscard]] FrameExecutionSlot& GetSlotLocked(FrameHandle frame);

    [[nodiscard]] const FrameExecutionSlot& GetSlotLocked(FrameHandle frame) const;

    static void
    TransitionLocked(FrameHandle frame, FrameExecutionSlot& slot, EFrameState expectedState, EFrameState nextState);

private:
    mutable std::mutex m_mutex;

    const std::uint64_t m_ownerId;
    const std::size_t m_maxActiveFrames;

    std::unique_ptr<FrameExecutionSlot[]> m_slots;

    std::uint64_t m_nextFrameIndex = 0;
};

} // namespace NEngine::NController
