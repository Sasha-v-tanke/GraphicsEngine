#pragma once

#include <cstddef>

namespace NEngine {

struct EngineConfig {
    std::size_t MaxActiveFrames = 2;
    std::size_t WorkerCount = 0;
};

} // namespace NEngine
