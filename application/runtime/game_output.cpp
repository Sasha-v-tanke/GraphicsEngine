#include "game_output.h"

#include <lib/common/error/exception.h>

#if defined(GRAPHICS_ENGINE_HAS_PRESENTATION)
    #include <renderer/presentation/presentation.h>
    #include <renderer/standard_graph.h>
#endif

namespace NApplication::NRuntime {
namespace {
thread_local GameOutputFactory g_factory = nullptr;

#if defined(GRAPHICS_ENGINE_HAS_PRESENTATION)
class GraphOutput final: public IGameOutput {
public:
    GraphOutput(NWindow::Window& window, const NGraphics::PresentationConfig& config)
        : m_window(window)
        , m_config(config) {
        Start();
    }
    void Start() override {
        if (!m_graphics) {
            m_graphics = std::make_unique<NRenderer::GraphPresentation>(m_window, m_config);
        }
    }
    void SetPresentationSize(int width, int height) override {
        m_graphics->SetPresentationSize(width, height);
    }
    NGraphics::FrameSubmissionResult TrySubmit(const NRenderer::RenderWorld&,
                                               const NRenderer::RenderPlan& plan) override {
        // F-0095 publishes the selected view; F-0096 adds prepared opaque/depth submission.
        // Until then this composition deliberately uses the builder's explicit no-camera clear path.
        return m_graphics->Render(plan.GetFrame(), [&](const auto& target) {
            return NRenderer::BuildStandardGraph(plan.GetFrame(), plan, std::nullopt, target);
        });
    }
    [[nodiscard]] bool IsCompleted(NGraphics::CompletionPoint completion) const override {
        return m_graphics->IsCompleted(completion);
    }
    void Stop() noexcept override {
        m_graphics.reset();
    }

private:
    NWindow::Window& m_window;
    NGraphics::PresentationConfig m_config;
    std::unique_ptr<NRenderer::GraphPresentation> m_graphics;
};
#endif
} // namespace

void SetGameOutputFactoryForTests(GameOutputFactory factory) noexcept {
    g_factory = factory;
}

std::unique_ptr<IGameOutput> CreateGameOutput(NWindow::Window& window, const NGraphics::PresentationConfig& config) {
    if (g_factory) {
        auto output = g_factory(window, config);
        if (!output) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Game output factory returned null");
        }
        return output;
    }
#if defined(GRAPHICS_ENGINE_HAS_PRESENTATION)
    return std::make_unique<GraphOutput>(window, config);
#else
    (void)window;
    (void)config;
    GRAPHICS_ENGINE_THROW(NCommon::EError::UNSUPPORTED, "Standard 3D Game requires Vulkan and GLFW");
#endif
}

} // namespace NApplication::NRuntime
