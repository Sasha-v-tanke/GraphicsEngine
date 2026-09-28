#pragma once

#include <memory>

#include <graphics/completion_point.h>
#include <graphics/frame_submission.h>
#include <graphics/graphics_capabilities.h>
#include <graphics/graphics_config.h>
#include <lib/common/wrapper/non_transferable.h>

namespace NGraphics::NBackend {

class IGraphicsBackend;

} // namespace NGraphics::NBackend

namespace NGraphics {

class Graphics final: public NCommon::NonTransferable {
public:
    explicit Graphics(const GraphicsConfig& config = {});
    ~Graphics();

    [[nodiscard]] const GraphicsCapabilities& GetCapabilities() const noexcept;

    [[nodiscard]] CompletionPoint SubmitFrame(const FrameSubmission& submission);
    [[nodiscard]] bool IsCompleted(CompletionPoint completion) const;

private:
    std::unique_ptr<NBackend::IGraphicsBackend> m_backend;
};

} // namespace NGraphics
