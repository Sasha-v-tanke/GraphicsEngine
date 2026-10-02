#pragma once

#include <cstdint>
#include <optional>

#include <graphics/buffer.h>
#include <graphics/frame_submission.h>
#include <graphics/graphics_capabilities.h>
#include <lib/common/wrapper/non_transferable.h>

namespace NGraphics::NBackend {

class IGraphicsBackend: public NCommon::NonTransferable {
public:
    virtual ~IGraphicsBackend() = default;

    [[nodiscard]] virtual const GraphicsCapabilities& GetCapabilities() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t SubmitFrame(FrameSubmission submission) = 0;
    [[nodiscard]] virtual bool IsCompleted(std::uint64_t completionValue) const = 0;
    [[nodiscard]] virtual std::uint64_t CreateBuffer(const BufferDescriptor& descriptor) = 0;
    virtual void DestroyBuffer(std::uint64_t bufferValue, std::optional<std::uint64_t> completedAfter) noexcept = 0;
};

} // namespace NGraphics::NBackend
