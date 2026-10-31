#pragma once

#include <functional>
#include <memory>

#include <GraphicsEngine/graphics/frame_submission.h>
#include <GraphicsEngine/graphics/material.h>
#include <GraphicsEngine/graphics/presentation.h>
#include <GraphicsEngine/renderer/render_graph.h>
#include <GraphicsEngine/resources/mesh_data.h>

namespace NRenderer {

// Main-thread serial graph executor. Window outlives this object. Prepare immutable
// asset versions before frames; their CPU/GPU representations stay pinned until shutdown.
// Available with Vulkan+GLFW. No native handles cross this boundary.
class GraphPresentation final {
public:
    GraphPresentation(NWindow::Window& window, const NGraphics::PresentationConfig& config);
    ~GraphPresentation();
    GraphPresentation(const GraphPresentation&) = delete;
    GraphPresentation& operator=(const GraphPresentation&) = delete;

    void PrepareMesh(NResources::ResourceLease<NResources::MeshData> mesh);
    // Descriptor-free R3 baseline; bindings are resolved by the later R4 descriptor path.
    // VertexPushConstantBytes=64 passes the column-major WorldTransform at offset zero;
    // zero means the shader does not consume a transform. Other ranges are rejected.
    void PrepareMaterial(NResources::ResourceLease<NGraphics::Material> material,
                         NGraphics::GraphicsPipelineDescriptor pipeline);
    [[nodiscard]] NGraphics::EPixelFormat GetColorFormat() const;
    void SetPresentationSize(int width, int height);

    // Called only after successful acquire, with a fresh target for that acquired image.
    // The producer must return a graph for exactly frame and this target. It is invoked
    // once on submission, never on NOT_READY/SUSPENDED retries. It must not mutate assets.
    // Graph declarations are consumed synchronously; prepared assets survive GPU completion.
    using BuildGraph = std::function<CompiledGraph(const std::shared_ptr<const SwapchainGraphTarget>&)>;
    [[nodiscard]] NGraphics::FrameSubmissionResult Render(RenderFrameIdentity frame, const BuildGraph& build);
    [[nodiscard]] bool IsCompleted(NGraphics::CompletionPoint completion) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    std::uint64_t m_owner;
};
} // namespace NRenderer
