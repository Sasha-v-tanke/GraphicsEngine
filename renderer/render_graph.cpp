#include "render_graph.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>

#include <GraphicsEngine/graphics/graphics.h>
#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>

namespace NRenderer {
namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "{}", message);
    }
}

std::uint64_t AcquireGraphIdentity() {
    static std::atomic<std::uint64_t> next{1};
    std::uint64_t value = next.load(std::memory_order_relaxed);
    do {
        Require(value != std::numeric_limits<std::uint64_t>::max(), "Graph identity space exhausted");
    } while (!next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
    return value;
}

bool IsRead(EResourceUsage usage) {
    switch (usage) {
    case EResourceUsage::SAMPLED_READ:
    case EResourceUsage::VERTEX_READ:
    case EResourceUsage::INDEX_READ:
    case EResourceUsage::UNIFORM_READ:
    case EResourceUsage::TRANSFER_SOURCE:
        return true;
    default:
        return false;
    }
}

bool IsWrite(EResourceUsage usage) {
    return usage == EResourceUsage::COLOR_ATTACHMENT_WRITE || usage == EResourceUsage::DEPTH_STENCIL_WRITE ||
           usage == EResourceUsage::TRANSFER_DESTINATION;
}

bool IsDepth(NGraphics::EPixelFormat format) {
    return format == NGraphics::EPixelFormat::D32_FLOAT || format == NGraphics::EPixelFormat::D24_UNORM_S8_UINT;
}

void ValidateImage(const GraphImageDescription& image) {
    Require(image.Extent.Width > 0 && image.Extent.Height > 0 && image.Extent.Depth == 1,
            "Graph images must have a nonzero 2D extent");
    switch (image.Format) {
    case NGraphics::EPixelFormat::RGBA8_UNORM:
    case NGraphics::EPixelFormat::BGRA8_UNORM:
    case NGraphics::EPixelFormat::RGBA8_SRGB:
    case NGraphics::EPixelFormat::BGRA8_SRGB:
    case NGraphics::EPixelFormat::RGBA16_FLOAT:
    case NGraphics::EPixelFormat::D32_FLOAT:
    case NGraphics::EPixelFormat::D24_UNORM_S8_UINT:
        break;
    default:
        Require(false, "Unsupported graph image format");
    }
    switch (image.Samples) {
    case NGraphics::ESampleCount::X1:
    case NGraphics::ESampleCount::X2:
    case NGraphics::ESampleCount::X4:
    case NGraphics::ESampleCount::X8:
        break;
    default:
        Require(false, "Invalid graph sample count");
    }
    constexpr auto known = NGraphics::EImageUsage::TransferSource | NGraphics::EImageUsage::TransferDestination |
                           NGraphics::EImageUsage::Sampled | NGraphics::EImageUsage::ColorAttachment |
                           NGraphics::EImageUsage::DepthStencilAttachment;
    Require(image.Usage != 0 && (image.Usage & ~known) == 0, "Invalid graph image usage flags");
    const auto incompatible = IsDepth(image.Format) ? NGraphics::EImageUsage::ColorAttachment
                                                    : NGraphics::EImageUsage::DepthStencilAttachment;
    Require((image.Usage & NGraphics::ImageUsage(incompatible)) == 0,
            "Image format and attachment capability disagree");
}

NGraphics::EPixelFormat PixelFormat(NGraphics::EImageFormat format) {
    switch (format) {
    case NGraphics::EImageFormat::RGBA8_UNORM:
        return NGraphics::EPixelFormat::RGBA8_UNORM;
    case NGraphics::EImageFormat::BGRA8_UNORM:
        return NGraphics::EPixelFormat::BGRA8_UNORM;
    case NGraphics::EImageFormat::D32_FLOAT:
        return NGraphics::EPixelFormat::D32_FLOAT;
    default:
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Unsupported Graphics image format for graph");
    }
}

GraphImageDescription DescribeImage(const NGraphics::ImageDescriptor& image) {
    return {image.Extent, PixelFormat(image.Format), NGraphics::ESampleCount::X1, image.Usage};
}

void ValidateUsage(const ImportedGraphResource& resource, EResourceUsage usage, bool allowUndefined) {
    if (usage == EResourceUsage::UNDEFINED) {
        Require(allowUndefined, "Undefined final/pass usage");
        return;
    }
    if (usage == EResourceUsage::PRESENT) {
        Require(resource.Kind == EGraphResourceKind::SWAPCHAIN, "Only swapchain images may be presented");
        return;
    }
    if (resource.Kind == EGraphResourceKind::BUFFER) {
        NGraphics::EBufferUsage required;
        switch (usage) {
        case EResourceUsage::VERTEX_READ:
            required = NGraphics::EBufferUsage::Vertex;
            break;
        case EResourceUsage::INDEX_READ:
            required = NGraphics::EBufferUsage::Index;
            break;
        case EResourceUsage::UNIFORM_READ:
            required = NGraphics::EBufferUsage::Uniform;
            break;
        case EResourceUsage::TRANSFER_SOURCE:
            required = NGraphics::EBufferUsage::TransferSource;
            break;
        case EResourceUsage::TRANSFER_DESTINATION:
            required = NGraphics::EBufferUsage::TransferDestination;
            break;
        default:
            Require(false, "Unsupported buffer usage");
            return;
        }
        Require((resource.BufferDescription.Usage & NGraphics::BufferUsage(required)) != 0,
                "Buffer lacks required usage capability");
        const auto access = IsRead(usage) ? NGraphics::EBufferAccess::GpuRead : NGraphics::EBufferAccess::GpuWrite;
        Require((resource.BufferDescription.Access & NGraphics::BufferAccess(access)) != 0,
                "Buffer lacks required GPU access");
        return;
    }
    NGraphics::EImageUsage required;
    switch (usage) {
    case EResourceUsage::COLOR_ATTACHMENT_WRITE:
        required = NGraphics::EImageUsage::ColorAttachment;
        break;
    case EResourceUsage::DEPTH_STENCIL_WRITE:
        required = NGraphics::EImageUsage::DepthStencilAttachment;
        break;
    case EResourceUsage::SAMPLED_READ:
        required = NGraphics::EImageUsage::Sampled;
        break;
    case EResourceUsage::TRANSFER_SOURCE:
        required = NGraphics::EImageUsage::TransferSource;
        break;
    case EResourceUsage::TRANSFER_DESTINATION:
        required = NGraphics::EImageUsage::TransferDestination;
        break;
    default:
        Require(false, "Unsupported image usage");
        return;
    }
    Require((resource.ImageDescription.Usage & NGraphics::ImageUsage(required)) != 0,
            "Image lacks required usage capability");
    if (resource.Graphics != nullptr) {
        const auto image = resource.Graphics->GetImageDescriptor(resource.Image);
        const auto access = IsRead(usage) ? NGraphics::EImageAccess::GpuRead : NGraphics::EImageAccess::GpuWrite;
        Require((image.Access & NGraphics::ImageAccess(access)) != 0, "Image lacks required GPU access");
    }
}

void ValidateImport(const ImportedGraphResource& resource) {
    if (resource.Kind == EGraphResourceKind::SWAPCHAIN) {
        const auto target = resource.Swapchain.lock();
        Require(target && target->IsValid(), "Swapchain import was released or invalidated");
        Require(resource.FinalUsage == EResourceUsage::PRESENT, "Swapchain final usage must be PRESENT");
        Require(resource.InitialUsage == EResourceUsage::UNDEFINED || resource.InitialUsage == EResourceUsage::PRESENT,
                "Swapchain initial usage must describe acquired image state");
    } else if (resource.Kind == EGraphResourceKind::IMAGE) {
        (void)resource.Graphics->GetImageDescriptor(resource.Image);
    } else {
        (void)resource.Graphics->GetBufferDescriptor(resource.Buffer);
    }
    if (resource.Kind != EGraphResourceKind::BUFFER) {
        ValidateImage(resource.ImageDescription);
    }
    ValidateUsage(resource, resource.InitialUsage, true);
    ValidateUsage(resource, resource.FinalUsage, false);
}

} // namespace

SwapchainGraphTarget::SwapchainGraphTarget(std::uint64_t identity, GraphImageDescription description)
    : m_identity(identity)
    , m_description(description) {
    Require(identity != 0, "Swapchain identity must be nonzero");
    ValidateImage(description);
    Require(!IsDepth(description.Format) && description.Samples == NGraphics::ESampleCount::X1,
            "Swapchain must be a single-sample color image");
}

void SwapchainGraphTarget::Invalidate() noexcept {
    m_isValid = false;
}
bool SwapchainGraphTarget::IsValid() const noexcept {
    return m_isValid;
}
std::uint64_t SwapchainGraphTarget::GetIdentity() const noexcept {
    return m_identity;
}
const GraphImageDescription& SwapchainGraphTarget::GetDescription() const noexcept {
    return m_description;
}

CompiledGraph::CompiledGraph(RenderFrameIdentity frame)
    : m_frame(frame) {
}
RenderFrameIdentity CompiledGraph::GetFrame() const noexcept {
    return m_frame;
}
std::span<const ImportedGraphResource> CompiledGraph::GetResources() const noexcept {
    return m_resources;
}

const ImportedGraphResource& CompiledGraph::GetResource(GraphResourceId resource) const {
    Require(resource.m_index < m_resources.size() && m_resources[resource.m_index].Id == resource,
            "Invalid or foreign compiled resource");
    return m_resources[resource.m_index];
}
std::span<const CompiledGraphicsPass> CompiledGraph::GetPasses() const noexcept {
    return m_passes;
}
std::span<const GraphDependency> CompiledGraph::GetDependencies() const noexcept {
    return m_dependencies;
}

void CompiledGraph::ValidateResources(RenderFrameIdentity frame) const {
    Require(frame == m_frame, "Graph belongs to a different frame slot/generation");
    for (const ImportedGraphResource& resource: m_resources) {
        ValidateImport(resource);
    }
}

RenderGraphBuilder::RenderGraphBuilder(RenderFrameIdentity frame)
    : m_frame(frame)
    , m_identity(AcquireGraphIdentity()) {
    Require(frame.Generation != 0, "Graph requires a live frame generation");
}

GraphResourceId RenderGraphBuilder::AddResource(ImportedGraphResource resource) {
    for (const ImportedGraphResource& existing: m_resources) {
        if (existing.Kind != resource.Kind) {
            continue;
        }
        if (resource.Kind == EGraphResourceKind::SWAPCHAIN) {
            const auto left = existing.Swapchain.lock();
            const auto right = resource.Swapchain.lock();
            Require(!left || !right || left->GetIdentity() != right->GetIdentity(), "Duplicate swapchain import");
        } else if (existing.Graphics == resource.Graphics) {
            Require(resource.Kind == EGraphResourceKind::IMAGE ? existing.Image != resource.Image
                                                               : existing.Buffer != resource.Buffer,
                    "Duplicate resource import would hide hazards");
        }
    }
    GraphResourceId id;
    id.m_graph = m_identity;
    id.m_index = m_resources.size();
    resource.Id = id;
    ValidateImport(resource);
    m_resources.push_back(std::move(resource));
    return id;
}

GraphResourceId RenderGraphBuilder::ImportImage(const NGraphics::Graphics& graphics,
                                                NGraphics::ImageHandle image,
                                                EResourceUsage initialUsage,
                                                EResourceUsage finalUsage) {
    ImportedGraphResource resource;
    resource.Kind = EGraphResourceKind::IMAGE;
    resource.Graphics = &graphics;
    resource.Image = image;
    resource.ImageDescription = DescribeImage(graphics.GetImageDescriptor(image));
    resource.InitialUsage = initialUsage;
    resource.FinalUsage = finalUsage;
    return AddResource(std::move(resource));
}

GraphResourceId RenderGraphBuilder::ImportBuffer(const NGraphics::Graphics& graphics,
                                                 NGraphics::BufferHandle buffer,
                                                 EResourceUsage initialUsage,
                                                 EResourceUsage finalUsage) {
    ImportedGraphResource resource;
    resource.Kind = EGraphResourceKind::BUFFER;
    resource.Graphics = &graphics;
    resource.Buffer = buffer;
    resource.BufferDescription = graphics.GetBufferDescriptor(buffer);
    resource.InitialUsage = initialUsage;
    resource.FinalUsage = finalUsage;
    return AddResource(std::move(resource));
}

GraphResourceId RenderGraphBuilder::ImportSwapchain(const std::shared_ptr<const SwapchainGraphTarget>& target,
                                                    EResourceUsage initialUsage) {
    Require(target != nullptr, "Null swapchain import");
    ImportedGraphResource resource;
    resource.Kind = EGraphResourceKind::SWAPCHAIN;
    resource.Swapchain = target;
    resource.ImageDescription = target->GetDescription();
    resource.InitialUsage = initialUsage;
    resource.FinalUsage = EResourceUsage::PRESENT;
    return AddResource(std::move(resource));
}

GraphPassId RenderGraphBuilder::AddGraphicsPass(GraphicsPassDescription description) {
    GraphPassId id;
    id.m_graph = m_identity;
    id.m_index = m_passes.size();
    m_passes.push_back(std::move(description));
    return id;
}

std::size_t RenderGraphBuilder::Resolve(GraphResourceId resource) const {
    Require(resource.m_graph == m_identity && resource.m_index < m_resources.size(),
            "Invalid or foreign graph resource");
    return resource.m_index;
}
std::size_t RenderGraphBuilder::Resolve(GraphPassId pass) const {
    Require(pass.m_graph == m_identity && pass.m_index < m_passes.size(), "Invalid or foreign graph pass");
    return pass.m_index;
}
void RenderGraphBuilder::AddDependency(GraphPassId before, GraphPassId after) {
    Require(Resolve(before) != Resolve(after), "Self dependency");
    m_dependencies.push_back({before, after});
}

CompiledGraph RenderGraphBuilder::Compile() const {
    CompiledGraph result{m_frame};
    result.m_resources = m_resources;
    result.ValidateResources(m_frame);
    const std::size_t count = m_passes.size();
    const std::size_t none = count;
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    edges.reserve(m_dependencies.size());
    for (const GraphDependency& edge: m_dependencies) {
        edges.emplace_back(Resolve(edge.Before), Resolve(edge.After));
    }
    std::vector<std::size_t> writers(m_resources.size(), none);
    std::vector<std::vector<std::size_t>> readers(m_resources.size());
    // Declaration order versions each whole resource. Explicit edges may reorder
    // independent passes, but cannot reverse a declared RAW/WAR/WAW hazard.
    for (std::size_t index = 0; index < count; ++index) {
        const GraphicsPassDescription& pass = m_passes[index];
        std::vector<bool> used(m_resources.size());
        const auto registerUse = [&](const GraphResourceUse& use, bool isWrite) {
            const std::size_t resource = Resolve(use.Resource);
            Require(!used[resource], "Conflicting or duplicate use of a resource in one pass");
            used[resource] = true;
            Require(isWrite ? IsWrite(use.Usage) : IsRead(use.Usage), "Usage does not match read/write declaration");
            ValidateUsage(m_resources[resource], use.Usage, false);
            if (writers[resource] != none) {
                edges.emplace_back(writers[resource], index);
            }
            if (isWrite) {
                for (const std::size_t reader: readers[resource]) {
                    edges.emplace_back(reader, index);
                }
                readers[resource].clear();
                writers[resource] = index;
            } else {
                readers[resource].push_back(index);
            }
        };
        for (const GraphResourceUse& read: pass.Reads) {
            registerUse(read, false);
        }
        for (const GraphResourceUse& write: pass.Writes) {
            registerUse(write, true);
        }
        std::vector<bool> attached(m_resources.size());
        const GraphImageDescription* first = nullptr;
        const auto validateAttachment = [&](const GraphAttachment& attachment, bool isDepth) {
            const std::size_t resource = Resolve(attachment.Resource);
            const ImportedGraphResource& imported = m_resources[resource];
            Require(imported.Kind != EGraphResourceKind::BUFFER && !attached[resource], "Invalid/duplicate attachment");
            attached[resource] = true;
            const EResourceUsage usage =
                    isDepth ? EResourceUsage::DEPTH_STENCIL_WRITE : EResourceUsage::COLOR_ATTACHMENT_WRITE;
            Require(std::ranges::any_of(pass.Writes,
                                        [&](const GraphResourceUse& use) {
                                            return use.Resource == attachment.Resource && use.Usage == usage;
                                        }),
                    "Attachment must have a matching write declaration");
            Require(attachment.Load == EAttachmentLoad::LOAD || attachment.Load == EAttachmentLoad::CLEAR ||
                            attachment.Load == EAttachmentLoad::DISCARD,
                    "Invalid attachment load policy");
            Require(attachment.Store == EAttachmentStore::STORE || attachment.Store == EAttachmentStore::DISCARD,
                    "Invalid attachment store policy");
            if (attachment.Load == EAttachmentLoad::LOAD && imported.Graphics != nullptr) {
                const NGraphics::ImageDescriptor image = imported.Graphics->GetImageDescriptor(imported.Image);
                Require((image.Access & NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead)) != 0,
                        "Attachment LOAD requires GPU read access");
            }
            Require(std::ranges::all_of(attachment.ClearColor, [](float value) { return std::isfinite(value); }) &&
                            std::isfinite(attachment.ClearDepth) && attachment.ClearDepth >= 0.0F &&
                            attachment.ClearDepth <= 1.0F,
                    "Invalid attachment clear value");
            Require(imported.ImageDescription.Format != NGraphics::EPixelFormat::D32_FLOAT ||
                            attachment.ClearStencil == 0,
                    "D32 has no stencil aspect");
            if (first != nullptr) {
                const auto& image = imported.ImageDescription;
                Require(first->Extent.Width == image.Extent.Width && first->Extent.Height == image.Extent.Height &&
                                first->Samples == image.Samples,
                        "Attachment extent/sample mismatch");
            } else {
                first = &imported.ImageDescription;
            }
        };
        for (const GraphAttachment& attachment: pass.ColorAttachments) {
            validateAttachment(attachment, false);
        }
        if (pass.DepthAttachment) {
            validateAttachment(*pass.DepthAttachment, true);
        }
        Require(pass.DrawCommands.empty() || first != nullptr, "Draw packets require attachments");
        for (const GraphResourceUse& write: pass.Writes) {
            if (write.Usage == EResourceUsage::COLOR_ATTACHMENT_WRITE ||
                write.Usage == EResourceUsage::DEPTH_STENCIL_WRITE) {
                Require(attached[Resolve(write.Resource)], "Attachment write lacks attachment description");
            }
        }
    }
    std::ranges::sort(edges);
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<std::vector<std::size_t>> outgoing(count);
    std::vector<std::size_t> incoming(count);
    for (const auto& [before, after]: edges) {
        outgoing[before].push_back(after);
        ++incoming[after];
        GraphPassId source;
        source.m_graph = m_identity;
        source.m_index = before;
        GraphPassId destination;
        destination.m_graph = m_identity;
        destination.m_index = after;
        result.m_dependencies.push_back({source, destination});
    }
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
    for (std::size_t index = 0; index < count; ++index) {
        if (incoming[index] == 0) {
            ready.push(index);
        }
    }
    std::vector<bool> initialized;
    initialized.reserve(m_resources.size());
    for (const ImportedGraphResource& resource: m_resources) {
        initialized.push_back(resource.InitialUsage != EResourceUsage::UNDEFINED);
    }
    while (!ready.empty()) {
        const std::size_t index = ready.top();
        ready.pop();
        const auto& pass = m_passes[index];
        for (const GraphResourceUse& read: pass.Reads) {
            Require(initialized[Resolve(read.Resource)], "Read before produce");
        }
        const auto checkLoad = [&](const GraphAttachment& attachment) {
            Require(attachment.Load != EAttachmentLoad::LOAD || initialized[Resolve(attachment.Resource)],
                    "Attachment LOAD before produce");
        };
        for (const GraphAttachment& attachment: pass.ColorAttachments) {
            checkLoad(attachment);
        }
        if (pass.DepthAttachment) {
            checkLoad(*pass.DepthAttachment);
        }
        for (const GraphResourceUse& write: pass.Writes) {
            initialized[Resolve(write.Resource)] = true;
        }
        const auto applyStore = [&](const GraphAttachment& attachment) {
            initialized[Resolve(attachment.Resource)] = attachment.Store == EAttachmentStore::STORE;
        };
        for (const GraphAttachment& attachment: pass.ColorAttachments) {
            applyStore(attachment);
        }
        if (pass.DepthAttachment) {
            applyStore(*pass.DepthAttachment);
        }
        GraphPassId id;
        id.m_graph = m_identity;
        id.m_index = index;
        result.m_passes.push_back({id, pass});
        for (const std::size_t next: outgoing[index]) {
            if (--incoming[next] == 0) {
                ready.push(next);
            }
        }
    }
    Require(result.m_passes.size() == count, "RenderGraph contains a cycle");
    for (std::size_t resource = 0; resource < m_resources.size(); ++resource) {
        Require(initialized[resource], "Final usage requires initialized resource contents");
    }
    return result;
}

} // namespace NRenderer
