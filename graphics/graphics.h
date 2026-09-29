#pragma once

#include <cstdint>
#include <memory>

#include <graphics/completion_point.h>
#include <graphics/frame_submission.h>
#include <graphics/graphics_capabilities.h>
#include <graphics/graphics_config.h>
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

private:
    explicit Graphics(std::unique_ptr<NBackend::IGraphicsBackend> backend,
                      const RequiredGraphicsCapabilities& requiredCapabilities = {});

    [[nodiscard]] static std::uint64_t AcquireOwnerId();

    std::unique_ptr<NBackend::IGraphicsBackend> m_backend;
    std::uint64_t m_ownerId = 0;

    friend Graphics NBackend::CreateGraphicsForBackend(std::unique_ptr<NBackend::IGraphicsBackend> backend,
                                                       const RequiredGraphicsCapabilities& requiredCapabilities);
};

} // namespace NGraphics
