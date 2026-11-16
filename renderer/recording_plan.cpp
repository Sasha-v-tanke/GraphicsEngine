#include "recording_plan.h"

#include <algorithm>

#include <lib/common/error/exception.h>

namespace NRenderer::NInternal {

std::vector<GraphRecordingRange> PlanRecordingChunks(std::span<const GraphRecordingRange> ranges, std::size_t workers) {
    if (workers == 0) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Recording requires at least one worker");
    }
    std::vector<GraphRecordingRange> chunks;
    for (std::size_t begin = 0; begin < ranges.size();) {
        std::size_t end = begin + 1;
        while (end < ranges.size() && ranges[end].PassIndex == ranges[begin].PassIndex) {
            ++end;
        }
        const std::size_t count = end - begin;
        const std::size_t parallelism = std::min(workers, count);
        const std::size_t limit = parallelism >= count - parallelism ? count : 2 * parallelism;
        const std::size_t stride = 1 + (count - 1) / limit;
        for (std::size_t first = begin; first < end;) {
            const std::size_t last = first + std::min(stride, end - first) - 1;
            chunks.push_back({ranges[first].PassIndex,
                              ranges[first].FirstDraw,
                              ranges[last].FirstDraw + ranges[last].DrawCount - ranges[first].FirstDraw});
            first = last + 1;
        }
        begin = end;
    }
    return chunks;
}

} // namespace NRenderer::NInternal
