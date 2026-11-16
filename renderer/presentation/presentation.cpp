#include "presentation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <graphics/backend/completion_access.h>
#include <graphics/backend/submission_uses.h>
#include <graphics/pipeline_key.h>
#include <graphics/vulkan/clear_present_path.h>
#include <graphics/vulkan/descriptor_manager.h>
#include <graphics/vulkan/glfw_surface.h>
#include <graphics/vulkan/memory.h>
#include <graphics/vulkan/pipeline_manager.h>
#include <graphics/vulkan/rendering_scope.h>
#include <graphics/vulkan/resource_conversion.h>
#include <graphics/vulkan/upload_manager.h>
#include <lib/common/error/exception.h>
#include <math/transform.h>
#include <renderer/execution_validation.h>
#include <renderer/presentation/graph_state.h>
#include <renderer/recording_plan.h>

namespace NRenderer {
namespace {

[[noreturn]] void Invalid(std::string_view message) {
    GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "{}", message);
}

NVulkan::VulkanInstanceConfig InstanceConfig(const NGraphics::PresentationConfig& config) {
    if (config.MaxActiveFrames == 0 || config.MaxActiveFrames > std::numeric_limits<std::uint32_t>::max())
        Invalid("Invalid active frame count");
    NVulkan::VulkanInstanceConfig result{};
    result.RequiredExtensions = NVulkan::NGlfw::GetRequiredInstanceExtensions();
    result.ValidationMode = config.RequireValidation ? NVulkan::EValidationMode::Required
                                                     : NVulkan::EValidationMode::EnabledIfAvailable;
    result.EnableDebugUtils = true;
    result.DebugMessageHandler = [handler = config.ValidationErrorHandler](const NVulkan::VulkanDebugMessage& message) {
        if (handler && (message.Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
            handler(message.Message);
    };
    return result;
}

NCommon::TaskSystem* RecordingExecutor(GraphRecordingConfig config) {
    switch (config.Mode) {
    case EGraphRecordingMode::SERIAL:
        return nullptr;
    case EGraphRecordingMode::PARALLEL:
        if (config.Executor != nullptr) {
            return config.Executor;
        }
        Invalid("Parallel recording requires an external TaskSystem");
    }
    Invalid("Invalid graph recording mode");
}

NGraphics::EPixelFormat ColorFormat(VkFormat format) {
    constexpr std::array formats{
            std::pair{VK_FORMAT_B8G8R8A8_SRGB, NGraphics::EPixelFormat::BGRA8_SRGB},
            std::pair{VK_FORMAT_B8G8R8A8_UNORM, NGraphics::EPixelFormat::BGRA8_UNORM},
            std::pair{VK_FORMAT_R8G8B8A8_SRGB, NGraphics::EPixelFormat::RGBA8_SRGB},
            std::pair{VK_FORMAT_R8G8B8A8_UNORM, NGraphics::EPixelFormat::RGBA8_UNORM},
    };
    for (const auto& [native, neutral]: formats) {
        if (native == format)
            return neutral;
    }
    GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Unsupported presentation format");
}

VkFormat VulkanColorFormat(NGraphics::EPixelFormat format) {
    switch (format) {
    case NGraphics::EPixelFormat::RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case NGraphics::EPixelFormat::BGRA8_UNORM:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case NGraphics::EPixelFormat::RGBA8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case NGraphics::EPixelFormat::BGRA8_SRGB:
        return VK_FORMAT_B8G8R8A8_SRGB;
    default:
        Invalid("Unsupported secondary color format");
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
    Impl(NWindow::Window& window, const NGraphics::PresentationConfig& config, GraphRecordingConfig recording)
        : Executor(RecordingExecutor(recording))
        , Instance(InstanceConfig(config))
        , Surface(Instance, window)
        , Physical(NVulkan::SelectVulkanPhysicalDevice(Instance, Surface.GetHandle()))
        , Device(Physical)
        , Memory(Instance.GetHandle(), Physical.Handle, Device, States)
        , Pipelines(std::make_shared<NVulkan::VulkanPipelineManager>(Device, Physical))
        , Descriptors(
                  std::make_shared<NVulkan::VulkanDescriptorManager>(Device.GetHandle(), Device.GetDescriptorLimits()))
        , Transients(config.MaxActiveFrames)
        , Swapchain(Device, Physical, Surface.GetHandle(), window.GetFramebufferSize())
        , Path(Executor ? NVulkan::VulkanClearPresentPath(Device, Swapchain, config.MaxActiveFrames, *Executor)
                        : NVulkan::VulkanClearPresentPath(Device, Swapchain, config.MaxActiveFrames))
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
        NVulkan::VulkanDescriptorSetLease Descriptors;
        NGraphics::NBackend::SubmissionUseSet ResourceUses;
    };

    struct Image {
        VkDevice Device = VK_NULL_HANDLE;
        NResources::ResourceIdentity Identity;
        NResources::ResourceLease<NResources::ImageData> Lease;
        NVulkan::VulkanImageAllocation Allocation;
        VkImageView View = VK_NULL_HANDLE;
        ~Image() {
            vkDestroyImageView(Device, View, nullptr);
        }
    };
    struct Sampler {
        VkDevice Device = VK_NULL_HANDLE;
        NResources::ResourceIdentity Identity;
        NResources::ResourceLease<NGraphics::SamplerDescriptor> Lease;
        VkSampler Handle = VK_NULL_HANDLE;
        ~Sampler() {
            vkDestroySampler(Device, Handle, nullptr);
        }
    };

    // Path is destroyed first and drains GPU/WSI before prepared resources disappear.
    NCommon::TaskSystem* Executor;
    NVulkan::VulkanInstance Instance;
    NVulkan::NGlfw::VulkanSurface Surface;
    NVulkan::VulkanPhysicalDeviceSelection Physical;
    NVulkan::VulkanDevice Device;
    NVulkan::VulkanResourceStateTracker States;
    NVulkan::VulkanMemoryAllocator Memory;
    std::shared_ptr<NVulkan::VulkanPipelineManager> Pipelines;
    std::vector<std::shared_ptr<Image>> Images;
    std::vector<std::shared_ptr<Sampler>> Samplers;
    // All sets and their resources stay immutable and alive until Path drains.
    std::shared_ptr<NVulkan::VulkanDescriptorManager> Descriptors;
    std::vector<Mesh> Meshes;
    std::vector<Material> Materials;
    struct TransientImage {
        VkDevice Device = VK_NULL_HANDLE;
        GraphImageDescription Description;
        NVulkan::VulkanImageAllocation Allocation;
        VkImageView View = VK_NULL_HANDLE;
        ~TransientImage() {
            vkDestroyImageView(Device, View, nullptr);
        }
    };
    // Each ring slot is reused only after Path observes its exact GPU completion.
    // Shared use records also retain replaced allocations across failed present.
    std::vector<std::vector<std::shared_ptr<TransientImage>>> Transients;
    NVulkan::VulkanSwapchain Swapchain;
    NVulkan::VulkanClearPresentPath Path;
    std::size_t MaxFrames;
    std::uint64_t TargetIdentity = 0;
    std::uint64_t NextFrame = 0;
    const std::thread::id Thread = std::this_thread::get_id();
    void CheckThread() const {
        if (Thread != std::this_thread::get_id() || NCommon::TaskSystem::IsCurrentThreadWorker()) {
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

    std::shared_ptr<TransientImage>
    AcquireTransient(std::size_t slot, std::size_t allocation, const GraphImageDescription& description) {
        auto& pool = Transients.at(slot);
        if (allocation >= pool.size()) {
            pool.resize(allocation + 1);
        }
        auto& cached = pool[allocation];
        if (cached && cached->Description.Extent.Width == description.Extent.Width &&
            cached->Description.Extent.Height == description.Extent.Height &&
            cached->Description.Format == description.Format && cached->Description.Usage == description.Usage) {
            return cached;
        }
        auto created = std::make_shared<TransientImage>();
        created->Device = Device.GetHandle();
        created->Description = description;
        const NGraphics::ImageDescriptor image{.Extent = description.Extent,
                                               .Format = description.Format == NGraphics::EPixelFormat::RGBA8_UNORM
                                                               ? NGraphics::EImageFormat::RGBA8_UNORM
                                                               : NGraphics::EImageFormat::BGRA8_UNORM,
                                               .Usage = description.Usage,
                                               .Access = NGraphics::EImageAccess::GpuRead |
                                                         NGraphics::EImageAccess::GpuWrite,
                                               .Lifetime = NGraphics::EImageLifetime::FrameLocal};
        created->Allocation = NVulkan::CreateVulkanImage(Memory, image);
        const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .pNext = nullptr,
                                         .flags = 0,
                                         .image = created->Allocation.GetImage(),
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = NVulkan::ToVulkanFormat(image.Format),
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        Check(vkCreateImageView(Device.GetHandle(), &view, nullptr, &created->View));
        cached = created;
        return created;
    }

    void RecordDraws(VkCommandBuffer command, VkExtent2D renderExtent, std::span<const DrawCommandData> draws) const {
        const VkViewport viewport{0,
                                  0,
                                  static_cast<float>(renderExtent.width),
                                  static_cast<float>(renderExtent.height),
                                  0,
                                  1};
        const VkRect2D scissor{.offset = {}, .extent = renderExtent};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        for (const DrawCommandData& draw: draws) {
            const auto& mesh = FindMesh(draw);
            const auto& material = FindMaterial(draw);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, material.Pipeline->GetHandle());
            if (material.Descriptors.IsValid()) {
                VkDescriptorSet set = material.Descriptors.GetSet();
                vkCmdBindDescriptorSets(command,
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        material.Pipeline->GetLayout(),
                                        0,
                                        1,
                                        &set,
                                        0,
                                        nullptr);
            }
            VkBuffer vertex = mesh.Vertices.GetBuffer();
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(command, 0, 1, &vertex, &offset);
            vkCmdBindIndexBuffer(command, mesh.Indices.GetBuffer(), 0, VK_INDEX_TYPE_UINT32);
            if (material.Key.GetDescriptor().VertexPushConstantBytes != 0) {
                auto transform = draw.ViewProjection *
                                 draw.ResolvedWorldTransform.value_or(NMath::ComposeTransform(draw.WorldTransform));
                // Engine clip space is Y-up; Vulkan uses a positive-height framebuffer viewport.
                for (std::size_t column = 0; column < 4; ++column) {
                    transform(1, column) = -transform(1, column);
                }
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

    struct RecordingChunk {
        GraphRecordingRange Range;
        bool Secondary = false;
        VkCommandBuffer Command = VK_NULL_HANDLE;
    };

    std::vector<RecordingChunk> RecordSecondaries(const CompiledGraph& graph, RenderFrameIdentity frame) {
        if (Executor == nullptr) {
            return {};
        }
        std::vector<RecordingChunk> chunks;
        for (const GraphRecordingRange& range:
             NInternal::PlanRecordingChunks(graph.GetRecordingRanges(), Executor->GetWorkerCount())) {
            // One compiler range (up to 64 draws) is cheaper inline. A pass is either entirely
            // inline or entirely secondary: Vulkan 1.3 does not mix the two rendering contents.
            chunks.push_back({range, graph.GetPasses()[range.PassIndex].Description.DrawCommands.size() > 64});
        }
        const std::size_t workCount = static_cast<std::size_t>(
                std::ranges::count_if(chunks, [](const RecordingChunk& chunk) { return chunk.Secondary; }));
        if (workCount == 0) {
            return chunks;
        }
        std::atomic_size_t next{0};
        const NCommon::TaskGroupHandle group = Executor->CreateTaskGroup();
        try {
            for (std::size_t worker = 0; worker < std::min(Executor->GetWorkerCount(), workCount); ++worker) {
                Executor->SubmitToGroup(group, [this, &graph, frame, &chunks, &next](NCommon::TaskContext& task) {
                    Path.WithWorkerContext(
                            {frame.ApplicationFrameIndex, frame.FrameSlotIndex},
                            task,
                            [&](NVulkan::VulkanWorkerContext& context) {
                                for (;;) {
                                    const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                                    if (index >= chunks.size()) {
                                        return;
                                    }
                                    RecordingChunk& chunk = chunks[index];
                                    if (!chunk.Secondary) {
                                        continue;
                                    }
                                    if (task.IsGroupCancellationRequested()) {
                                        return;
                                    }
                                    const auto& pass = graph.GetPasses()[chunk.Range.PassIndex].Description;
                                    const auto& image =
                                            graph.GetResource(pass.ColorAttachments[0].Resource).ImageDescription;
                                    const VkFormat format = VulkanColorFormat(image.Format);
                                    const VkCommandBufferInheritanceRenderingInfo rendering{
                                            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
                                            .pNext = nullptr,
                                            .flags = 0,
                                            .viewMask = 0,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &format,
                                            .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
                                            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
                                            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                                    };
                                    const VkCommandBufferInheritanceInfo inheritance{
                                            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
                                            .pNext = &rendering,
                                            .renderPass = VK_NULL_HANDLE,
                                            .subpass = 0,
                                            .framebuffer = VK_NULL_HANDLE,
                                            .occlusionQueryEnable = VK_FALSE,
                                            .queryFlags = 0,
                                            .pipelineStatistics = 0,
                                    };
                                    const VkCommandBufferBeginInfo begin{
                                            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .pNext = nullptr,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                                                     VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
                                            .pInheritanceInfo = &inheritance,
                                    };
                                    const VkCommandBuffer commands = context.AcquireSecondaryCommandBuffer();
                                    Check(vkBeginCommandBuffer(commands, &begin));
                                    RecordDraws(commands,
                                                {image.Extent.Width, image.Extent.Height},
                                                std::span{pass.DrawCommands}.subspan(chunk.Range.FirstDraw,
                                                                                     chunk.Range.DrawCount));
                                    Check(vkEndCommandBuffer(commands));
                                    chunk.Command = commands;
                                }
                            });
                });
            }
            Executor->SealGroup(group);
        } catch (...) {
            // Submission may fail after siblings have started borrowing pools/graph storage.
            // Cancel + join on the host before stack unwinding releases any recording inputs.
            Executor->CancelGroup(group);
            Executor->WaitGroup(group);
            throw;
        }
        Executor->WaitGroup(group);
        if (Executor->GetGroupStatus(group) != NCommon::ETaskGroupStatus::COMPLETED) {
            const auto error = Executor->GetGroupError(group);
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "Secondary recording failed: {}",
                                  error ? error->Message : "cancelled");
        }
        return chunks;
    }

    void Record(VkCommandBuffer command,
                VkImage image,
                VkImageView view,
                const NVulkan::VulkanSwapchainPlan& plan,
                RenderFrameIdentity frame,
                const BuildGraph& build,
                NGraphics::NBackend::SubmissionUseSet& uses) {
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
        for (const auto& pass: graph.GetPasses()) {
            for (const DrawCommandData& draw: pass.Description.DrawCommands) {
                const Mesh& mesh = FindMesh(draw);
                const Material& material = FindMaterial(draw);
                uses.push_back(mesh.Vertices.GetRetentionToken());
                uses.push_back(mesh.Indices.GetRetentionToken());
                uses.insert(uses.end(), material.ResourceUses.begin(), material.ResourceUses.end());
            }
        }
        const auto chunks = RecordSecondaries(graph, frame);
        std::vector<std::pair<GraphResourceId, std::shared_ptr<TransientImage>>> transients;
        for (const GraphResourceLifetime& lifetime: graph.GetLifetimes()) {
            if (lifetime.Allocation) {
                auto transient = AcquireTransient(frame.FrameSlotIndex,
                                                  *lifetime.Allocation,
                                                  graph.GetResource(lifetime.Resource).ImageDescription);
                uses.push_back(transient);
                transients.emplace_back(lifetime.Resource, std::move(transient));
            }
        }
        const auto nativeImage = [&](GraphResourceId resource) {
            const auto found = std::ranges::find(transients, resource, [](const auto& entry) { return entry.first; });
            return found == transients.end() ? image : found->second->Allocation.GetImage();
        };
        const auto nativeView = [&](GraphResourceId resource) {
            const auto found = std::ranges::find(transients, resource, [](const auto& entry) { return entry.first; });
            return found == transients.end() ? view : found->second->View;
        };
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .pNext = nullptr,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                             .pInheritanceInfo = nullptr};
        Check(vkBeginCommandBuffer(command, &begin));
        NVulkan::VulkanResourceStateTracker tracker;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        tracker.ImportImage(image, range, NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::Undefined));
        for (const auto& entry: transients) {
            tracker.ImportImage(entry.second->Allocation.GetImage(),
                                range,
                                NVulkan::MakeVulkanImageState(NVulkan::EVulkanResourceUsage::Undefined));
        }
        std::size_t transitionIndex = 0;
        const auto transitions = graph.GetTransitions();
        const auto applyTransitions = [&](std::size_t passIndex) {
            while (transitionIndex < transitions.size() && transitions[transitionIndex].PassIndex == passIndex) {
                const GraphTransition& transition = transitions[transitionIndex++];
                if (const auto barrier = tracker.TransitionImage(
                            {nativeImage(transition.Resource),
                             NInternal::VulkanUsage(transition.After, transition.ReadsAttachment),
                             range})) {
                    NVulkan::RecordVulkanResourceBarrier(command, *barrier);
                }
            }
        };
        std::size_t chunkIndex = 0;
        for (std::size_t passIndex = 0; passIndex < graph.GetPasses().size(); ++passIndex) {
            const auto& pass = graph.GetPasses()[passIndex];
            applyTransitions(passIndex);
            const auto& color = pass.Description.ColorAttachments[0];
            const auto& extent = graph.GetResource(color.Resource).ImageDescription.Extent;
            const VkExtent2D renderExtent{extent.Width, extent.Height};
            VkClearColorValue clear{};
            std::copy(color.ClearColor.begin(), color.ClearColor.end(), clear.float32);
            const NVulkan::VulkanColorAttachment attachment{nativeView(color.Resource),
                                                            Load(color.Load),
                                                            Store(color.Store),
                                                            clear};
            const bool parallelPass = chunkIndex < chunks.size() && chunks[chunkIndex].Range.PassIndex == passIndex &&
                                      chunks[chunkIndex].Secondary;
            const NVulkan::VulkanRenderingScope rendering{
                    command,
                    {.offset = {}, .extent = renderExtent},
                    std::span{&attachment, 1},
                    parallelPass ? NVulkan::ERenderingContents::SECONDARY_COMMAND_BUFFERS
                                 : NVulkan::ERenderingContents::INLINE};
            if (parallelPass) {
                while (chunkIndex < chunks.size() && chunks[chunkIndex].Range.PassIndex == passIndex) {
                    const VkCommandBuffer secondary = chunks[chunkIndex++].Command;
                    vkCmdExecuteCommands(command, 1, &secondary);
                }
            } else {
                RecordDraws(command, renderExtent, pass.Description.DrawCommands);
                while (chunkIndex < chunks.size() && chunks[chunkIndex].Range.PassIndex == passIndex) {
                    ++chunkIndex;
                }
            }
        }
        applyTransitions(graph.GetPasses().size());
        Check(vkEndCommandBuffer(command));
    }
};

GraphPresentation::GraphPresentation(NWindow::Window& window,
                                     const NGraphics::PresentationConfig& config,
                                     GraphRecordingConfig recording)
    : m_impl(std::make_unique<Impl>(window, config, recording))
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
    if (!material.IsValid() || (pipeline.VertexPushConstantBytes != 0 && pipeline.VertexPushConstantBytes != 64) ||
        pipeline.Samples != NGraphics::ESampleCount::X1 ||
        pipeline.DepthAttachmentFormat != NGraphics::EPixelFormat::UNDEFINED ||
        pipeline.Topology != NGraphics::EPrimitiveTopology::TRIANGLE_LIST ||
        pipeline.VertexLayout.Bindings !=
                std::vector<NGraphics::VertexBindingDescriptor>{
                        {0, sizeof(NResources::MeshVertex), NGraphics::EVertexInputRate::VERTEX}}) {
        Invalid("Unsupported material/pipeline layout");
    }
    const std::vector layout(material->GetLayout().begin(), material->GetLayout().end());
    if (!pipeline.MaterialBindings.empty() && pipeline.MaterialBindings != layout)
        Invalid("Pipeline bindings differ from the material layout");
    pipeline.MaterialBindings = layout;
    NGraphics::PipelineKey key{std::move(pipeline)};
    for (const auto& entry: m_impl->Materials) {
        if (entry.Lease.GetResource() == material.GetResource() && entry.Lease.GetVersion() == material.GetVersion()) {
            if (entry.Key != key)
                Invalid("Material version already prepared with another pipeline");
            return;
        }
    }
    const auto& prepared = m_impl->Pipelines->Prepare(key);
    NGraphics::NBackend::SubmissionUseSet resourceUses{m_impl->Pipelines, m_impl->Descriptors};
    std::vector<NVulkan::VulkanResolvedMaterialBinding> bindings;
    for (const auto& binding: material->GetBindings()) {
        NVulkan::VulkanResolvedMaterialBinding resolved{};
        resolved.Material = binding;
        if (binding.Type == NGraphics::EMaterialBindingType::UniformBuffer ||
            binding.Type == NGraphics::EMaterialBindingType::StorageBuffer)
            Invalid("Graph presentation does not prepare material buffers yet");
        if (binding.Type != NGraphics::EMaterialBindingType::Sampler) {
            const auto& identity = binding.Type == NGraphics::EMaterialBindingType::CombinedImageSampler
                                         ? binding.CombinedImageSampler.Image
                                         : binding.Image.Resource;
            const auto found = std::find_if(m_impl->Images.rbegin(), m_impl->Images.rend(), [&](const auto& image) {
                return image->Identity == identity;
            });
            if (found == m_impl->Images.rend())
                Invalid("Material image was not prepared");
            resolved.Image = {(*found)->View, (*found)->Lease.GetVersion()};
            resourceUses.push_back(*found);
        }
        if (binding.Type != NGraphics::EMaterialBindingType::SampledImage) {
            const auto& identity = binding.Type == NGraphics::EMaterialBindingType::CombinedImageSampler
                                         ? binding.CombinedImageSampler.Sampler
                                         : binding.Sampler.Resource;
            const auto found = std::find_if(m_impl->Samplers.rbegin(),
                                            m_impl->Samplers.rend(),
                                            [&](const auto& sampler) { return sampler->Identity == identity; });
            if (found == m_impl->Samplers.rend())
                Invalid("Material sampler was not prepared");
            resolved.Sampler = {(*found)->Handle, (*found)->Lease.GetVersion()};
            resourceUses.push_back(*found);
        }
        bindings.push_back(resolved);
    }
    NVulkan::VulkanDescriptorSetLease descriptors;
    if (!layout.empty())
        descriptors = m_impl->Descriptors->Acquire({layout, bindings});
    m_impl->Materials.push_back({std::move(material), std::move(key), &prepared, descriptors, std::move(resourceUses)});
}

void GraphPresentation::PrepareImage(NResources::ResourceManager& resources,
                                     NResources::ResourceHandle<NResources::ImageData> handle) {
    m_impl->CheckThread();
    auto lease = resources.TryAcquire(handle);
    if (!lease || resources.GetIdentity(handle).GetResourceClass() != "image")
        Invalid("Image is not ready or has the wrong resource class");
    const auto width = (*lease)->GetWidth();
    const auto height = (*lease)->GetHeight();
    if (width == 0 || height == 0 || (*lease)->GetFormat() != NResources::EImagePixelFormat::RGBA8 ||
        static_cast<std::uint64_t>(width) * height > std::numeric_limits<std::size_t>::max() / 4 ||
        (*lease)->GetPixels().size() != static_cast<std::size_t>(width) * height * 4)
        Invalid("Invalid prepared RGBA8 image");
    for (const auto& image: m_impl->Images) {
        if (image->Lease.GetResource() == handle && image->Lease.GetVersion() == lease->GetVersion())
            return;
    }
    auto image = std::make_shared<Impl::Image>();
    image->Device = m_impl->Device.GetHandle();
    image->Identity = resources.GetIdentity(handle);
    image->Lease = std::move(*lease);
    image->Allocation = NVulkan::CreateVulkanImage(
            m_impl->Memory,
            {.Extent = {width, height, 1},
             .Format = NGraphics::EImageFormat::RGBA8_UNORM,
             .Usage = NGraphics::EImageUsage::TransferDestination | NGraphics::EImageUsage::Sampled,
             .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead)});
    // This startup upload owns a separate timeline. Frame completion remains Path-owned.
    NVulkan::VulkanSubmissionManager submissions{m_impl->Device};
    NVulkan::VulkanUploadManager uploads{m_impl->Device, m_impl->Memory, submissions, m_impl->States};
    const auto completion = uploads.UploadImage(image->Allocation, std::as_bytes(std::span{image->Lease->GetPixels()}));
    submissions.Wait(completion, std::numeric_limits<std::uint64_t>::max());
    uploads.ReleaseCompleted();
    NGraphics::ImageViewDescriptor view{};
    view.Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color);
    image->View = m_impl->Device.CreateImageView(image->Allocation.GetImage(), view);
    m_impl->Images.push_back(std::move(image));
}

void GraphPresentation::PrepareSampler(NResources::ResourceManager& resources,
                                       NResources::ResourceHandle<NGraphics::SamplerDescriptor> handle) {
    m_impl->CheckThread();
    auto lease = resources.TryAcquire(handle);
    if (!lease || resources.GetIdentity(handle).GetResourceClass() != "sampler")
        Invalid("Sampler is not ready or has the wrong resource class");
    const auto& desc = **lease;
    if (!std::isfinite(desc.MinLod) || !std::isfinite(desc.MaxLod) || desc.MinLod < 0 || desc.MaxLod < desc.MinLod)
        Invalid("Invalid sampler LOD range");
    for (const auto& sampler: m_impl->Samplers) {
        if (sampler->Lease.GetResource() == handle && sampler->Lease.GetVersion() == lease->GetVersion())
            return;
    }
    auto sampler = std::make_shared<Impl::Sampler>();
    sampler->Device = m_impl->Device.GetHandle();
    sampler->Identity = resources.GetIdentity(handle);
    sampler->Lease = std::move(*lease);
    sampler->Handle = m_impl->Device.CreateSampler(desc);
    m_impl->Samplers.push_back(std::move(sampler));
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
    NGraphics::NBackend::SubmissionUseSet resourceUses;
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
                    m_impl->Record(command, image, view, plan, frame, build, resourceUses);
                },
                resourceUses);
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
