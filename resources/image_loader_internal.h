#pragma once

#include <cstddef>
#include <limits>

namespace NResources::NImageLoaderInternal {

[[nodiscard]] constexpr bool CanPassImageByteSizeToStb(std::size_t byteSize) noexcept {
    return byteSize <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

} // namespace NResources::NImageLoaderInternal
