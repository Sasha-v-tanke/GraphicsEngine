#pragma once

#include <graphics/vulkan/resource_state.h>
#include <renderer/render_graph.h>

namespace NRenderer::NInternal {
// One lowering for compiled graph semantics; the shared tracker owns native hazards.
[[nodiscard]] NVulkan::EVulkanResourceUsage VulkanUsage(EResourceUsage usage, bool readsAttachment = false);
} // namespace NRenderer::NInternal
