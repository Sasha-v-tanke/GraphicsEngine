#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>

#include <GraphicsEngine/renderer/render_graph.h>

namespace NRenderer {

struct StandardGraphOutputs {
    GraphResourceId Color;
    std::optional<GraphResourceId> Depth;
    GraphPassId OpaquePass;
};

struct StandardGraphOptions {
    std::array<float, 4> ClearColor{0.0F, 0.0F, 0.0F, 1.0F};
    EResourceUsage InitialColorUsage = EResourceUsage::UNDEFINED;
    // Synchronous, frame-local extension. Declare semantic passes/dependencies;
    // do not retain the builder. Outputs belong only to this invocation.
    std::function<void(RenderGraphBuilder&, const StandardGraphOutputs&)> Extend;
};

// CPU-only declaration/compile. The plan's frame must match exactly. nullopt
// camera produces clear-only; no implicit first-camera fallback. Commands already
// contain the selected camera matrix and ready logical resource versions from
// Renderer::Prepare. GPU preparation/execution remains the caller's responsibility.
// Target must be live and nonzero (suspended outputs must not call this function).
// WSI target and the frame's resource leases must survive execution/completion,
// following CompiledGraph's lifetime contract. Throws on invalid/stale inputs.
[[nodiscard]] CompiledGraph BuildStandardGraph(RenderFrameIdentity frame,
                                               const RenderPlan& plan,
                                               std::optional<RenderObjectId> camera,
                                               const std::shared_ptr<const SwapchainGraphTarget>& target,
                                               const StandardGraphOptions& options = {});

} // namespace NRenderer
