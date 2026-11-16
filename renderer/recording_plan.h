#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <renderer/render_graph.h>

namespace NRenderer::NInternal {

// Coalesce adjacent compiler ranges into at most 2*workers chunks per pass.
// Pass boundaries, exact-once draw coverage and logical order are preserved.
[[nodiscard]] std::vector<GraphRecordingRange> PlanRecordingChunks(std::span<const GraphRecordingRange> ranges,
                                                                   std::size_t workers);

} // namespace NRenderer::NInternal
