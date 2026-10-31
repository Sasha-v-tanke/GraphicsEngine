#include "presentation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <graphics/backend/completion_access.h>
#include <graphics/pipeline_key.h>
#include <graphics/vulkan/clear_present_path.h>
#include <graphics/vulkan/glfw_surface.h>
#include <graphics/vulkan/memory.h>
#include <graphics/vulkan/pipeline_manager.h>
#include <graphics/vulkan/rendering_scope.h>
#include <lib/common/error/exception.h>
#include <math/transform.h>
#include <renderer/execution_validation.h>

namespace NRenderer {
namespace {

[[noreturn]] void Invalid(std::string_view message) {
    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "{}", message);
}

NVulkan::VulkanInstanceConfig InstanceConfig(const NGraphics::PresentationConfig& config) {
    if (config.MaxActiveFrames == 0 || config.MaxActiveFrames > std::numeric_limits<std::uint32_t>::max())
        Invalid("Invalid active frame count");
    return {.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions(),
            .ValidationMode = config.RequireValidation ? NVulkan::EValidationMode::Required
                                                       : NVulkan::EValidationMode::EnabledIfAvailable,
            .EnableDebugUtils = true,
            .DebugMessageHandler = [handler =
                                            config.ValidationErrorHandler](const NVulkan::VulkanDebugMessage& message) {
                if (handler && (message.Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
                    handler(message.Message);
                }
            }};
}

NGraphics::EPixelFormat ColorFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB:
        return NGraphics::EPixelFormat::BGRA8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return NGraphics::EPixelFormat::BGRA8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return NGraphics::EPixelFormat::RGBA8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return NGraphics::EPixelFormat::RGBA8_UNORM;
    default:
        GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Unsupported presentation format");
    }
}

VkAttachmentLoadOp Load(EAttachmentLoad load) {
    switch (load) {
    case EAttachmentLoad::LOAD:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case EAttachmentLoad::CLEAR:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case EAttachmentLoad::DISCARD:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    Invalid("Invalid attachment load operation");
}
VkAttachmentStoreOp Store(EAttachmentStore store) {
    switch (store) {
    case EAttachmentStore::STORE:
        return VK_ATTACHMENT_STORE_OP_STORE;
    case EAttachmentStore::DISCARD:
        return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    Invalid("Invalid attachment store operation");
}

void Check(VkResult result) {
    if (result != VK_SUCCESS) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                              "Vulkan graph recording failed: {}",
                              static_cast<int>(result));
    }
}
} // namespace

class GraphPresentation::Impl {
public:
    Impl(NWindow::Window& window, const NGraphics::PresentationConfig& config)
        : Instance(InstanceConfig(config))
        , Surface(Instance, window)
        , Physical(NVulkan::SelectVulkanPhysicalDevice(Instance, Surface.GetHandle()))
        , Device(Physical)
        , Memory(Instance.GetHandle(), Physical.Handle, Device, States)
        , Pipelines(Device, Physical)
        , Swapchain(Device, Physical, Surface.GetHandle(), window.GetFramebufferSize())
        , Path(Device, Swapchain, config.MaxActiveFrames)
        , MaxFrames(config.MaxActiveFrames) {
    }

    struct Mesh {
        NResources::ResourceLease<NResources::MeshData> Lease;
        NVulkan::VulkanBufferAllocation Vertices;
        NVulkan::VulkanBufferAllocation Indices;
    };
    struct Material {
        NResources::ResourceLease<NGraphics::Material> Lease;
        NGraphics::PipelineKey Key;
        const NVulkan::VulkanGraphicsPipeline* Pipeline;
    };

    // Path is destroyed first and drains GPU/WSI before prepared resources disappear.
    NVulkan::VulkanInstance Instance;
    NVulkan::NGlfw::VulkanSurface Surface;
    NVulkan::VulkanPhysicalDeviceSelection Physical;
    NVulkan::VulkanDevice Device;
    NVulkan::VulkanResourceStateTracker States;
    NVulkan::VulkanMemoryAllocator Memory;
    NVulkan::VulkanPipelineManager Pipelines;
    std::vector<Mesh> Meshes;
    std::vector<Material> Materials;
    NVulkan::VulkanSwapchain Swapchain;
    NVulkan::VulkanClearPresentPath Path;
    std::size_t MaxFrames;
    std::uint64_t TargetIdentity = 0;
    std::uint64_t NextFrame = 0;
    const std::thread::id Thread = std::this_thread::get_id();
    void CheckThread() const {
        if (Thread != std::this_thread::get_id()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Graph presentation requires its owning thread");
        }
    }

    const Mesh& FindMesh(const DrawCommandData& draw) const {
        const auto found = std::ranges::find_if(Meshes, [&](const Mesh& mesh) {
            return mesh.Lease.GetResource() == draw.Mesh && mesh.Lease.GetVersion() == draw.MeshVersion;
        });
        if (found == Meshes.end())
            Invalid("Draw mesh version was not prepared");
        return *found;
    }
    const Material& FindMaterial(const DrawCommandData& draw) const {
        const auto found = std::ranges::find_if(Materials, [&](const Material& material) {
            return material.Lease.GetResource() == draw.Material && material.Lease.GetVersion() == draw.MaterialVersion;
        });
        if (found == Materials.end())
            Invalid("Draw material version was not prepared");
        return *found;
    }

    void Record(VkCommandBuffer command,
                VkImage image,
                VkImageView view,
                const NVulkan::VulkanSwapchainPlan& plan,
                RenderFrameIdentity frame,
                const BuildGraph& build) {
        auto target = std::make_shared<SwapchainGraphTarget>(
                ++TargetIdentity,
                GraphImageDescription{{plan.Extent.width, plan.Extent.height, 1},
                                      ColorFormat(plan.SurfaceFormat.format),
                                      NGraphics::ESampleCount::X1,
                                      NGraphics::ImageUsage(NGraphics::EImageUsage::ColorAttachment)});
        struct InvalidateTarget {
            SwapchainGraphTarget& Target;
            ~InvalidateTarget() {
                Target.Invalidate();
            }
        } invalidate{*target};
        const auto graph = build(target);
        NInternal::ValidateColorExecution(
                graph,
                frame,
                target,
                [&](const DrawCommandData& draw) -> const NGraphics::GraphicsPipelineDescriptor& {
                    (void)FindMesh(draw);
                    return FindMaterial(draw).Key.GetDescriptor();
                });
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        Check(vkBeginCommandBuffer(command, &begin));
        NVulkan::VulkanResourceStateTracker tracker;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        tracker.ImportImage(image, range, NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::Undefined));
        for (const auto& pass: graph.GetPasses()) {
            if (const auto barrier = tracker.TransitionImage(
                        {image, NVulkan::EVulkanResourceUsage::ColorAttachmentReadWrite, range})) {
                NVulkan::RecordVulkanResourceBarrier(command, *barrier);
            }
            const auto& color = pass.Description.ColorAttachments[0];
            VkClearColorValue clear{};
            std::copy(color.ClearColor.begin(), color.ClearColor.end(), clear.float32);
            const NVulkan::VulkanColorAttachment attachment{view, Load(color.Load), Store(color.Store), clear};
            const NVulkan::VulkanRenderingScope rendering{command, {.extent = plan.Extent}, std::span{&attachment, 1}};
            const VkViewport viewport{0,
                                      0,
                                      static_cast<float>(plan.Extent.width),
                                      static_cast<float>(plan.Extent.height),
                                      0,
                                      1};
            const VkRect2D scissor{.extent = plan.Extent};
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdSetScissor(command, 0, 1, &scissor);
            for (const auto& draw: pass.Description.DrawCommands) {
                const auto& mesh = FindMesh(draw);
                const auto& material = FindMaterial(draw);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, material.Pipeline->GetHandle());
                VkBuffer vertex = mesh.Vertices.GetBuffer();
                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(command, 0, 1, &vertex, &offset);
                vkCmdBindIndexBuffer(command, mesh.Indices.GetBuffer(), 0, VK_INDEX_TYPE_UINT32);
                if (material.Key.GetDescriptor().VertexPushConstantBytes != 0) {
                    const auto transform = NMath::ComposeTransform(draw.WorldTransform);
                    vkCmdPushConstants(command,
                                       material.Pipeline->GetLayout(),
                                       VK_SHADER_STAGE_VERTEX_BIT,
                                       0,
                                       64,
                                       transform.GetValues().data());
                }
                vkCmdDrawIndexed(command, static_cast<std::uint32_t>(mesh.Lease->GetIndices().size()), 1, 0, 0, 0);
            }
        }
        if (const auto barrier = tracker.TransitionImage({image, NVulkan::EVulkanResourceUsage::Present, range})) {
            NVulkan::RecordVulkanResourceBarrier(command, *barrier);
        }
        Check(vkEndCommandBuffer(command));
    }
};

GraphPresentation::GraphPresentation(NWindow::Window& window, const NGraphics::PresentationConfig& config)
    : m_impl(std::make_unique<Impl>(window, config))
    , m_owner(NGraphics::NBackend::CompletionAccess::NewOwner()) {
}
GraphPresentation::~GraphPresentation() = default;

void GraphPresentation::PrepareMesh(NResources::ResourceLease<NResources::MeshData> mesh) {
    m_impl->CheckThread();
    if (!mesh.IsValid() || mesh->GetVertices().empty() || mesh->GetIndices().empty() ||
        mesh->GetIndices().size() % 3 != 0 || mesh->GetIndices().size() > std::numeric_limits<std::uint32_t>::max()) {
        Invalid("Invalid prepared indexed triangle mesh");
    }
    for (const auto& vertex: mesh->GetVertices()) {
        for (float value: vertex.Position) {
            if (!std::isfinite(value))
                Invalid("Nonfinite mesh position");
        }
    }
    for (auto index: mesh->GetIndices()) {
        if (index >= mesh->GetVertices().size())
            Invalid("Mesh index exceeds vertex count");
    }
    for (const auto& entry: m_impl->Meshes) {
        if (entry.Lease.GetResource() == mesh.GetResource() && entry.Lease.GetVersion() == mesh.GetVersion())
            return;
    }
    auto allocate = [&](auto& data, NGraphics::EBufferUsage usage) {
        const auto bytes = std::as_bytes(std::span{data});
        auto buffer = NVulkan::CreateVulkanBuffer(
                m_impl->Memory,
                {.SizeBytes = bytes.size(),
                 .Usage = NGraphics::BufferUsage(usage),
                 .Access = NGraphics::EBufferAccess::CpuWrite | NGraphics::EBufferAccess::GpuRead});
        std::memcpy(buffer.Map(), bytes.data(), bytes.size());
        buffer.Flush(0, bytes.size());
        buffer.Unmap();
        return buffer;
    };
    auto vertices = allocate(mesh->GetVertices(), NGraphics::EBufferUsage::Vertex);
    auto indices = allocate(mesh->GetIndices(), NGraphics::EBufferUsage::Index);
    m_impl->Meshes.push_back({std::move(mesh), std::move(vertices), std::move(indices)});
}

void GraphPresentation::PrepareMaterial(NResources::ResourceLease<NGraphics::Material> material,
                                        NGraphics::GraphicsPipelineDescriptor pipeline) {
    m_impl->CheckThread();
    if (!material.IsValid() || !material->GetLayout().empty() || !material->GetBindings().empty() ||
        (pipeline.VertexPushConstantBytes != 0 && pipeline.VertexPushConstantBytes != 64) ||
        pipeline.Samples != NGraphics::ESampleCount::X1 ||
        pipeline.DepthAttachmentFormat != NGraphics::EPixelFormat::UNDEFINED ||
        pipeline.Topology != NGraphics::EPrimitiveTopology::TRIANGLE_LIST ||
        pipeline.VertexLayout.Bindings !=
                std::vector<NGraphics::VertexBindingDescriptor>{
                        {0, sizeof(NResources::MeshVertex), NGraphics::EVertexInputRate::VERTEX}}) {
        Invalid("Unsupported R3 material/pipeline layout");
    }
    NGraphics::PipelineKey key{std::move(pipeline)};
    for (const auto& entry: m_impl->Materials) {
        if (entry.Lease.GetResource() == material.GetResource() && entry.Lease.GetVersion() == material.GetVersion()) {
            if (entry.Key != key)
                Invalid("Material version already prepared with another pipeline");
            return;
        }
    }
    const auto& prepared = m_impl->Pipelines.Prepare(key);
    m_impl->Materials.push_back({std::move(material), std::move(key), &prepared});
}

NGraphics::EPixelFormat GraphPresentation::GetColorFormat() const {
    m_impl->CheckThread();
    return ColorFormat(m_impl->Swapchain.GetPlan().SurfaceFormat.format);
}
void GraphPresentation::SetPresentationSize(int width, int height) {
    m_impl->CheckThread();
    m_impl->Path.SetFramebufferSize({width, height});
}
NGraphics::FrameSubmissionResult GraphPresentation::Render(RenderFrameIdentity frame, const BuildGraph& build) {
    m_impl->CheckThread();
    if (!build || frame.ApplicationFrameIndex != m_impl->NextFrame ||
        frame.FrameSlotIndex != frame.ApplicationFrameIndex % m_impl->MaxFrames) {
        Invalid("Invalid graph frame identity or producer");
    }
    NGraphics::FrameSubmissionResult result;
    try {
        (void)m_impl->Path.PollCompletedFrames();
        const auto submitted = m_impl->Path.Render(
                frame.ApplicationFrameIndex,
                {},
                [&](NVulkan::VulkanQueueCompletion completion) {
                    result.Completion = NGraphics::NBackend::CompletionAccess::Make(m_owner, completion.Value);
                    ++m_impl->NextFrame;
                },
                [&](VkCommandBuffer command,
                    VkImage image,
                    VkImageView view,
                    const NVulkan::VulkanSwapchainPlan& plan) {
                    m_impl->Record(command, image, view, plan, frame, build);
                });
        switch (submitted.Status) {
        case NVulkan::EVulkanClearPresentStatus::PRESENTED:
            result.Status = NGraphics::EFrameSubmissionStatus::PRESENTED;
            break;
        case NVulkan::EVulkanClearPresentStatus::NOT_READY:
            result.Status = NGraphics::EFrameSubmissionStatus::NOT_READY;
            break;
        case NVulkan::EVulkanClearPresentStatus::OUT_OF_DATE:
            result.Status = NGraphics::EFrameSubmissionStatus::OUT_OF_DATE;
            break;
        case NVulkan::EVulkanClearPresentStatus::SUSPENDED:
            result.Status = NGraphics::EFrameSubmissionStatus::SUSPENDED;
            break;
        }
    } catch (...) {
        result.Status = NGraphics::EFrameSubmissionStatus::FAILED;
        result.Failure = std::current_exception();
    }
    return result;
}
bool GraphPresentation::IsCompleted(NGraphics::CompletionPoint completion) const {
    m_impl->CheckThread();
    if (!completion.IsValid() || !NGraphics::NBackend::CompletionAccess::Owns(completion, m_owner))
        Invalid("Foreign graph completion");
    return m_impl->Path.IsCompleted({.Value = completion.GetValue()});
}
} // namespace NRenderer
