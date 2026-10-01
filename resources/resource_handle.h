#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace NResources {

class ResourceManager;

template<typename T>
class ResourceHandle final {
public:
    ResourceHandle() = default;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_ownerId != 0 && m_generation != 0 && m_slotIndex != INVALID_SLOT_INDEX;
    }

    [[nodiscard]] std::size_t GetSlotIndex() const noexcept {
        return m_slotIndex;
    }

    [[nodiscard]] std::uint64_t GetGeneration() const noexcept {
        return m_generation;
    }

    [[nodiscard]] friend bool operator==(ResourceHandle lhs, ResourceHandle rhs) noexcept = default;

private:
    static constexpr std::size_t INVALID_SLOT_INDEX = std::numeric_limits<std::size_t>::max();

    ResourceHandle(std::uint64_t ownerId, std::size_t slotIndex, std::uint64_t generation) noexcept
        : m_ownerId(ownerId)
        , m_slotIndex(slotIndex)
        , m_generation(generation) {
    }

    std::uint64_t m_ownerId = 0;
    std::size_t m_slotIndex = INVALID_SLOT_INDEX;
    std::uint64_t m_generation = 0;

    friend class ResourceManager;
};

template<typename T>
class ResourceOperation final {
public:
    ResourceOperation() = default;

    [[nodiscard]] bool IsValid() const noexcept {
        return m_handle.IsValid() && m_generation != 0;
    }

    [[nodiscard]] ResourceHandle<T> GetResource() const noexcept {
        return m_handle;
    }

    [[nodiscard]] std::uint64_t GetGeneration() const noexcept {
        return m_generation;
    }

private:
    ResourceOperation(ResourceHandle<T> handle, std::uint64_t generation) noexcept
        : m_handle(handle)
        , m_generation(generation) {
    }

    ResourceHandle<T> m_handle;
    std::uint64_t m_generation = 0;

    friend class ResourceManager;
};

} // namespace NResources
