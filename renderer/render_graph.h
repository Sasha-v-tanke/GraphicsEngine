#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <GraphicsEngine/graphics/graphics.h>
#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>
#include <GraphicsEngine/renderer/renderer.h>

namespace NRenderer {

enum class EResourceUsage {
    UNDEFINED,
    COLOR_ATTACHMENT_WRITE,
    DEPTH_STENCIL_WRITE,
    SAMPLED_READ,
    VERTEX_READ,
    INDEX_READ,
    UNIFORM_READ,
    TRANSFER_SOURCE,
    TRANSFER_DESTINATION,
    PRESENT,
};

enum class EAttachmentLoad {
    LOAD,
    CLEAR,
    DISCARD,
};
enum class EAttachmentStore {
    STORE,
    DISCARD,
};

struct GraphImageDescription {
    NGraphics::ImageExtent Extent;
    NGraphics::EPixelFormat Format = NGraphics::EPixelFormat::UNDEFINED;
    NGraphics::ESampleCount Samples = NGraphics::ESampleCount::X1;
    NGraphics::ImageUsageFlags Usage = 0;
};

// WSI owns this source through GPU completion. Invalidate before resize/release;
// a new acquired image/generation requires a new source. No native handle crosses this boundary.
class SwapchainGraphTarget final: public NCommon::NonTransferable {
public:
    SwapchainGraphTarget(std::uint64_t identity, GraphImageDescription description);
    void Invalidate() noexcept;
    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] std::uint64_t GetIdentity() const noexcept;
    [[nodiscard]] const GraphImageDescription& GetDescription() const noexcept;

private:
    std::uint64_t m_identity;
    GraphImageDescription m_description;
    bool m_isValid = true;
};

class GraphResourceId {
public:
    [[nodiscard]] friend bool operator==(GraphResourceId, GraphResourceId) noexcept = default;

private:
    std::uint64_t m_graph = 0;
    std::size_t m_index = 0;
    friend class RenderGraphBuilder;
    friend class CompiledGraph;
};

class GraphPassId {
public:
    [[nodiscard]] friend bool operator==(GraphPassId, GraphPassId) noexcept = default;

private:
    std::uint64_t m_graph = 0;
    std::size_t m_index = 0;
    friend class RenderGraphBuilder;
};

struct GraphResourceUse {
    GraphResourceId Resource;
    EResourceUsage Usage = EResourceUsage::UNDEFINED;
};

struct GraphAttachment {
    GraphResourceId Resource;
    EAttachmentLoad Load = EAttachmentLoad::CLEAR;
    EAttachmentStore Store = EAttachmentStore::STORE;
    std::array<float, 4> ClearColor{};
    float ClearDepth = 1.0F;
    std::uint32_t ClearStencil = 0;
};

struct GraphicsPassDescription {
    std::vector<GraphResourceUse> Reads;
    std::vector<GraphResourceUse> Writes;
    std::vector<GraphAttachment> ColorAttachments;
    std::optional<GraphAttachment> DepthAttachment;
    std::vector<DrawCommandData> DrawCommands;
};

enum class EGraphResourceKind {
    IMAGE,
    BUFFER,
    SWAPCHAIN,
};

struct ImportedGraphResource {
    GraphResourceId Id;
    EGraphResourceKind Kind = EGraphResourceKind::IMAGE;
    const NGraphics::Graphics* Graphics = nullptr;
    NGraphics::ImageHandle Image;
    NGraphics::BufferHandle Buffer;
    std::weak_ptr<const SwapchainGraphTarget> Swapchain;
    GraphImageDescription ImageDescription;
    NGraphics::BufferDescriptor BufferDescription;
    EResourceUsage InitialUsage = EResourceUsage::UNDEFINED;
    EResourceUsage FinalUsage = EResourceUsage::UNDEFINED;
};

struct GraphDependency {
    GraphPassId Before;
    GraphPassId After;
    [[nodiscard]] friend bool operator==(GraphDependency, GraphDependency) noexcept = default;
};

struct CompiledGraphicsPass {
    GraphPassId Id;
    GraphicsPassDescription Description;
};

// Immutable snapshot, owned by the frame slot. Destroy before slot recycle. It
// retains declarations, not GPU resources or logical resource leases. The frame's
// RenderWorld/use records and external Graphics/WSI owners must survive GPU completion.
class CompiledGraph final {
public:
    [[nodiscard]] RenderFrameIdentity GetFrame() const noexcept;
    [[nodiscard]] std::span<const ImportedGraphResource> GetResources() const noexcept;
    [[nodiscard]] const ImportedGraphResource& GetResource(GraphResourceId resource) const;
    [[nodiscard]] std::span<const CompiledGraphicsPass> GetPasses() const noexcept;
    [[nodiscard]] std::span<const GraphDependency> GetDependencies() const noexcept;
    // Executor must call this immediately before recording, on the owning thread.
    // Resource mutation/release and execution must be externally serialized.
    void ValidateResources(RenderFrameIdentity frame) const;

private:
    explicit CompiledGraph(RenderFrameIdentity frame);

private:
    RenderFrameIdentity m_frame;
    std::vector<ImportedGraphResource> m_resources;
    std::vector<CompiledGraphicsPass> m_passes;
    std::vector<GraphDependency> m_dependencies;
    friend class RenderGraphBuilder;
};

// Single-threaded frame-local declaration builder. Resource/pass IDs are scoped
// to this builder, including across recycled frame slots. Compile is CPU-only,
// repeatable and independent of later builder/descriptor mutations.
class RenderGraphBuilder final: public NCommon::NonTransferable {
public:
    explicit RenderGraphBuilder(RenderFrameIdentity frame);
    [[nodiscard]] GraphResourceId ImportImage(const NGraphics::Graphics& graphics,
                                              NGraphics::ImageHandle image,
                                              EResourceUsage initialUsage,
                                              EResourceUsage finalUsage);
    [[nodiscard]] GraphResourceId ImportBuffer(const NGraphics::Graphics& graphics,
                                               NGraphics::BufferHandle buffer,
                                               EResourceUsage initialUsage,
                                               EResourceUsage finalUsage);
    [[nodiscard]] GraphResourceId ImportSwapchain(const std::shared_ptr<const SwapchainGraphTarget>& target,
                                                  EResourceUsage initialUsage);
    [[nodiscard]] GraphPassId AddGraphicsPass(GraphicsPassDescription description);
    void AddDependency(GraphPassId before, GraphPassId after);
    [[nodiscard]] CompiledGraph Compile() const;

private:
    [[nodiscard]] std::size_t Resolve(GraphResourceId resource) const;
    [[nodiscard]] std::size_t Resolve(GraphPassId pass) const;
    [[nodiscard]] GraphResourceId AddResource(ImportedGraphResource resource);

private:
    RenderFrameIdentity m_frame;
    std::uint64_t m_identity;
    std::vector<ImportedGraphResource> m_resources;
    std::vector<GraphicsPassDescription> m_passes;
    std::vector<GraphDependency> m_dependencies;
};

} // namespace NRenderer
