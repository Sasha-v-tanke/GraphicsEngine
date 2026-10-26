#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace NScene {

class SceneManager;

class SceneHandle final {
public:
    SceneHandle() = default;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_ownerId != 0 && m_generation != 0 && m_slotIndex != INVALID_SLOT;
    }

    [[nodiscard]] std::size_t GetSlotIndex() const noexcept {
        return m_slotIndex;
    }

    [[nodiscard]] std::uint64_t GetGeneration() const noexcept {
        return m_generation;
    }

    [[nodiscard]] friend bool operator==(SceneHandle, SceneHandle) noexcept = default;

private:
    static constexpr std::size_t INVALID_SLOT = std::numeric_limits<std::size_t>::max();

    SceneHandle(std::uint64_t ownerId, std::size_t slotIndex, std::uint64_t generation) noexcept
        : m_ownerId(ownerId)
        , m_slotIndex(slotIndex)
        , m_generation(generation) {
    }

    std::uint64_t m_ownerId = 0;
    std::size_t m_slotIndex = INVALID_SLOT;
    std::uint64_t m_generation = 0;

    friend class SceneManager;
};

} // namespace NScene
