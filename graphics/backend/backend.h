#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include <graphics/buffer.h>
#include <graphics/frame_submission.h>
#include <graphics/graphics_capabilities.h>
#include <graphics/graphics_pipeline.h>
#include <graphics/image.h>
#include <lib/common/wrapper/non_transferable.h>

namespace NGraphics::NBackend {

struct BackendFrameResult {
    EFrameSubmissionStatus Status = EFrameSubmissionStatus::NOT_READY;
    std::uint64_t CompletionValue = 0;
    std::exception_ptr Failure = {};
};

class IGraphicsBackend: public NCommon::NonTransferable {
public:
    virtual ~IGraphicsBackend() = default;

    [[nodiscard]] virtual const GraphicsCapabilities& GetCapabilities() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t SubmitFrame(FrameSubmission submission) = 0;
    [[nodiscard]] virtual BackendFrameResult TrySubmitFrame(FrameSubmission submission) {
        return {.Status = EFrameSubmissionStatus::PRESENTED, .CompletionValue = SubmitFrame(std::move(submission))};
    }
    virtual void SetPresentationSize(int, int) {
    }
    [[nodiscard]] virtual bool IsCompleted(std::uint64_t completionValue) const = 0;
    [[nodiscard]] virtual std::uint64_t CreateBuffer(const BufferDescriptor& descriptor) = 0;
    virtual void DestroyBuffer(std::uint64_t bufferValue, std::optional<std::uint64_t> completedAfter) noexcept = 0;
    [[nodiscard]] virtual std::uint64_t CreateGraphicsPipeline(const GraphicsPipelineDescriptor& descriptor) = 0;
    virtual void DestroyGraphicsPipeline(std::uint64_t pipelineValue,
                                         std::optional<std::uint64_t> completedAfter) noexcept = 0;
    [[nodiscard]] virtual std::uint64_t CreateImage(const ImageDescriptor& descriptor) = 0;
    virtual void DestroyImage(std::uint64_t imageValue, std::optional<std::uint64_t> completedAfter) noexcept = 0;
    [[nodiscard]] virtual std::uint64_t CreateImageView(const ImageViewDescriptor& descriptor) = 0;
    virtual void DestroyImageView(std::uint64_t imageViewValue,
                                  std::optional<std::uint64_t> completedAfter) noexcept = 0;
    [[nodiscard]] virtual std::uint64_t CreateSampler(const SamplerDescriptor& descriptor) = 0;
    virtual void DestroySampler(std::uint64_t samplerValue, std::optional<std::uint64_t> completedAfter) noexcept = 0;
};

} // namespace NGraphics::NBackend
