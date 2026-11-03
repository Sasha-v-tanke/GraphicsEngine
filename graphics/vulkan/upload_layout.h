#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>

namespace NVulkan::NInternal {

// All slices share one submission. Atom-rounded non-coherent flushes cannot
// touch in-flight host writes because arenas retire as a whole.
inline std::uint64_t AppendUploadSlice(std::uint64_t& end, std::size_t size) {
    constexpr std::uint64_t alignment = 4;
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    if (end > maximum - (alignment - 1)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan upload batch size overflow");
    }
    const std::uint64_t offset = (end + alignment - 1) & ~(alignment - 1);
    if (size > maximum - offset) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Vulkan upload batch size overflow");
    }
    end = offset + size;
    return offset;
}

} // namespace NVulkan::NInternal
