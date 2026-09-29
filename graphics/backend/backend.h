#pragma once

#include <cstdint>

#include <graphics/frame_submission.h>
#include <graphics/graphics_capabilities.h>
#include <lib/common/wrapper/non_transferable.h>

namespace NGraphics::NBackend {

class IGraphicsBackend: public NCommon::NonTransferable {
public:
    virtual ~IGraphicsBackend() = default;

    [[nodiscard]] virtual const GraphicsCapabilities& GetCapabilities() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t SubmitFrame(const FrameSubmission& submission) = 0;
    [[nodiscard]] virtual bool IsCompleted(std::uint64_t completionValue) const = 0;
};

} // namespace NGraphics::NBackend
