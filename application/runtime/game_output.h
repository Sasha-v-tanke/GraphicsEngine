#pragma once

#include <memory>

#include <graphics/frame_submission.h>
#include <graphics/presentation.h>
#include <renderer/render_world.h>
#include <renderer/renderer.h>

namespace NApplication::NRuntime {

// Internal owner-thread output boundary. Only immutable frame data reaches the renderer.
// Stop destroys/drains native output before Window or borrowed services can disappear.
class IGameOutput {
public:
    virtual ~IGameOutput() = default;
    virtual void Start() = 0;
    virtual void SetPresentationSize(int width, int height) = 0;
    [[nodiscard]] virtual NGraphics::FrameSubmissionResult TrySubmit(const NRenderer::RenderWorld& world,
                                                                     const NRenderer::RenderPlan& plan) = 0;
    [[nodiscard]] virtual bool IsCompleted(NGraphics::CompletionPoint completion) const = 0;
    virtual void Stop() noexcept = 0;
};

using GameOutputFactory = std::unique_ptr<IGameOutput> (*)(NWindow::Window&, const NGraphics::PresentationConfig&);
// Owner-thread-only seam, matching Window's internal test factory. Not installed as public API.
void SetGameOutputFactoryForTests(GameOutputFactory factory) noexcept;
[[nodiscard]] std::unique_ptr<IGameOutput> CreateGameOutput(NWindow::Window& window,
                                                            const NGraphics::PresentationConfig& config);

} // namespace NApplication::NRuntime
