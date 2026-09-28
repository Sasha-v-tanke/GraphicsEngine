#pragma once

#include <graphics/completion_point.h>
#include <graphics/frame_submission.h>
#include <graphics/graphics_capabilities.h>
#include <lib/common/wrapper/non_transferable.h>

namespace NGraphics::NBackend {

class IGraphicsBackend: public NCommon::NonTransferable {
public:
    virtual ~IGraphicsBackend() = default;

    [[nodiscard]] virtual const GraphicsCapabilities& GetCapabilities() const noexcept = 0;
    [[nodiscard]] virtual CompletionPoint SubmitFrame(const FrameSubmission& submission) = 0;
    [[nodiscard]] virtual bool IsCompleted(CompletionPoint completion) const = 0;
};

} // namespace NGraphics::NBackend
