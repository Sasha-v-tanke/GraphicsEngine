#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace NResources::NMeshLoaderInternal {

[[nodiscard]] constexpr bool CanAppendMeshElements(std::size_t count, std::size_t additional) noexcept {
    constexpr std::size_t MAX_COUNT = std::numeric_limits<std::uint32_t>::max();
    return count <= MAX_COUNT && additional <= MAX_COUNT - count;
}

} // namespace NResources::NMeshLoaderInternal
