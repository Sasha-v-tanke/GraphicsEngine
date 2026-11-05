#pragma once

#include <functional>
#include <memory>

#include <GraphicsEngine/graphics/frame_submission.h>
#include <GraphicsEngine/graphics/image.h>
#include <GraphicsEngine/graphics/material.h>
#include <GraphicsEngine/graphics/presentation.h>
#include <GraphicsEngine/renderer/render_graph.h>
#include <GraphicsEngine/resources/image_data.h>
#include <GraphicsEngine/resources/mesh_data.h>
#include <GraphicsEngine/resources/resource_manager.h>

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
    // Startup-only preparation: immutable RGBA8 images and samplers stay pinned until
    // shutdown. Image upload completes before publication; no waits occur in Render.
    // Prepare referenced images/samplers before their material. Existing materials
    // retain their original versions when a new resource version is prepared.
    void PrepareImage(NResources::ResourceManager& resources, NResources::ResourceHandle<NResources::ImageData> handle);
    void PrepareSampler(NResources::ResourceManager& resources,
                        NResources::ResourceHandle<NGraphics::SamplerDescriptor> handle);
    // SampledImage/Sampler/CombinedImageSampler bindings use immutable set zero.
    // Buffer bindings remain available in DescriptorManager, outside this executor.
    // VertexPushConstantBytes=64 passes column-major ViewProjection * WorldTransform at offset zero;
    // Vulkan clip-space Y conversion is applied by the executor.
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
