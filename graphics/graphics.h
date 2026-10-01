#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <graphics/buffer.h>
#include <graphics/completion_point.h>
#include <graphics/frame_submission.h>
#include <graphics/graphics_capabilities.h>
#include <graphics/graphics_config.h>
#include <graphics/graphics_pipeline.h>
#include <lib/common/wrapper/non_transferable.h>

namespace NGraphics {

class Graphics;

namespace NBackend {

class IGraphicsBackend;
[[nodiscard]] Graphics CreateGraphicsForBackend(std::unique_ptr<IGraphicsBackend> backend,
                                                const RequiredGraphicsCapabilities& requiredCapabilities);

} // namespace NBackend

class Graphics final: public NCommon::NonTransferable {
public:
    explicit Graphics(const GraphicsConfig& config = {});
    ~Graphics();

    [[nodiscard]] const GraphicsCapabilities& GetCapabilities() const noexcept;

    [[nodiscard]] CompletionPoint SubmitFrame(const FrameSubmission& submission);
    [[nodiscard]] bool IsCompleted(CompletionPoint completion) const;

    [[nodiscard]] BufferHandle CreateBuffer(const BufferDescriptor& descriptor);
    void DestroyBuffer(BufferHandle buffer, CompletionPoint completedAfter = {});
    [[nodiscard]] BufferDescriptor GetBufferDescriptor(BufferHandle buffer) const;

    [[nodiscard]] GraphicsPipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDescriptor& descriptor);
    void DestroyGraphicsPipeline(GraphicsPipelineHandle pipeline, CompletionPoint completedAfter = {});
    [[nodiscard]] GraphicsPipelineDescriptor GetGraphicsPipelineDescriptor(GraphicsPipelineHandle pipeline) const;

private:
    explicit Graphics(std::unique_ptr<NBackend::IGraphicsBackend> backend,
                      const RequiredGraphicsCapabilities& requiredCapabilities = {});

    [[nodiscard]] static std::uint64_t AcquireOwnerId();

    struct BufferRecord {
        BufferDescriptor Descriptor;
        std::uint64_t Generation = 0;
    };

    struct GraphicsPipelineRecord {
        GraphicsPipelineDescriptor Descriptor;
        std::uint64_t Generation = 0;
    };

    [[nodiscard]] const BufferRecord& ResolveBuffer(BufferHandle buffer) const;
    [[nodiscard]] const GraphicsPipelineRecord& ResolveGraphicsPipeline(GraphicsPipelineHandle pipeline) const;
    void ValidateCompletionOwner(CompletionPoint completion) const;

    std::unique_ptr<NBackend::IGraphicsBackend> m_backend;
    std::uint64_t m_ownerId = 0;
    std::uint64_t m_nextBufferGeneration = 1;
    std::uint64_t m_nextGraphicsPipelineGeneration = 1;
    std::unordered_map<std::uint64_t, BufferRecord> m_buffers;
    std::unordered_map<std::uint64_t, GraphicsPipelineRecord> m_graphicsPipelines;

    friend Graphics NBackend::CreateGraphicsForBackend(std::unique_ptr<NBackend::IGraphicsBackend> backend,
                                                       const RequiredGraphicsCapabilities& requiredCapabilities);
};

} // namespace NGraphics
