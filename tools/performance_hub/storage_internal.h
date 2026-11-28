#pragma once

#include "tools/performance_hub/storage.h"

#include <istream>
#include <ostream>

namespace graphics_engine::performance_hub::detail {
struct CopiedArtifact {
    std::uintmax_t bytes;
    std::uint64_t hash;
};
// Internal streaming primitive: never relies on a pre-copy file size.
Result<CopiedArtifact> CopyArtifactBytes(std::istream& input, std::ostream& output, std::uintmax_t limit);
} // namespace graphics_engine::performance_hub::detail
