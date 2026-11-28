#pragma once

#include <cstdint>

namespace NGraphics {

enum class EGraphicsCapabilityProfile : std::uint8_t {
    BASE,
    ADVANCED,
    HIGH_END
};

struct GraphicsQueueTopologyCapabilities {
    bool Graphics = false;
    bool Compute = false;
    bool Transfer = false;
    bool DedicatedCompute = false;
    bool DedicatedTransfer = false;
};
struct GraphicsFormatCapabilities {
    bool Rgba8Unorm = false;
    bool D32Float = false;
    std::uint32_t MaxColorAttachmentSamples = 1;
    std::uint32_t MaxDepthStencilAttachmentSamples = 1;
};
struct GraphicsShaderComputeCapabilities {
    bool Compute = false;
    std::uint32_t MaxComputeWorkGroupInvocations = 0;
    std::uint32_t MaxComputeWorkGroupSizeX = 0;
    std::uint32_t MaxComputeWorkGroupSizeY = 0;
    std::uint32_t MaxComputeWorkGroupSizeZ = 0;
};
struct GraphicsDrawIndirectCapabilities {
    bool DrawIndirect = false;
    bool DrawIndirectCount = false;
    std::uint32_t MaxDrawIndirectCount = 0;
};
struct GraphicsBindingCapabilities {
    bool StorageBuffers = false;
    bool DescriptorIndexing = false;
    std::uint32_t MaxBoundDescriptorSets = 0;
    std::uint32_t MaxPerStageStorageBuffers = 0;
};
struct GraphicsMemoryCapabilities {
    std::uint64_t DeviceLocalMemoryBytes = 0;
    std::uint64_t MinUniformBufferOffsetAlignment = 0;
    std::uint64_t MinStorageBufferOffsetAlignment = 0;
};
struct GraphicsPresentationCapabilities {
    bool Presentation = false;
    bool SrgbColorSpace = false;
};
struct GraphicsProfilingCapabilities {
    bool TimestampQueries = false;
    float TimestampPeriodNanoseconds = 0.0F;
};

struct GraphicsCapabilities {
    // Compatibility fields stay first for positional aggregate initialization.
    bool Presentation = false;
    bool TimelineCompletion = false;
    std::uint32_t MaxFramesInFlight = 1;

    // Stable for the lifetime of a successfully initialized Graphics instance.
    std::uint64_t DeviceEpoch = 0;
    GraphicsQueueTopologyCapabilities QueueTopology;
    GraphicsFormatCapabilities Formats;
    GraphicsShaderComputeCapabilities ShaderCompute;
    GraphicsDrawIndirectCapabilities DrawIndirect;
    GraphicsBindingCapabilities Bindings;
    GraphicsMemoryCapabilities Memory;
    GraphicsPresentationCapabilities PresentationSupport;
    GraphicsProfilingCapabilities Profiling;
};

struct RequiredGraphicsCapabilities {
    // Compatibility fields stay first for positional aggregate initialization.
    bool Presentation = true;
    bool TimelineCompletion = false;
    std::uint32_t MaxFramesInFlight = 1;

    EGraphicsCapabilityProfile Profile = EGraphicsCapabilityProfile::BASE;
    bool Compute = false;
    bool DrawIndirect = false;
    bool DrawIndirectCount = false;
    bool StorageBuffers = false;
    bool DescriptorIndexing = false;
    bool TimestampQueries = false;
    std::uint32_t MinColorAttachmentSamples = 1;
    std::uint32_t MinDepthStencilAttachmentSamples = 1;
    std::uint32_t MinComputeWorkGroupInvocations = 0;
    std::uint32_t MinDrawIndirectCount = 0;
    std::uint32_t MinBoundDescriptorSets = 0;
    std::uint32_t MinPerStageStorageBuffers = 0;
    std::uint64_t MinDeviceLocalMemoryBytes = 0;
};

[[nodiscard]] RequiredGraphicsCapabilities
MakeRequiredGraphicsCapabilities(EGraphicsCapabilityProfile profile) noexcept;

[[nodiscard]] bool SatisfiesRequirements(const GraphicsCapabilities& capabilities,
                                         const RequiredGraphicsCapabilities& requirements) noexcept;

} // namespace NGraphics
