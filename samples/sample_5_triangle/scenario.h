#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

#include <ecs/world.h>
#include <engine/runtime/frame_runtime.h>
#include <renderer/extraction.h>
#include <renderer/presentation/presentation.h>
#include <renderer/render_components.h>
#include <resources/resource_manager.h>
#include <samples/sample_5_triangle/shaders.h>
#include <window/window.h>
#include <window/window_runtime.h>

namespace NTriangle {

struct Scene {
    NResources::ResourceManager Resources;
    NEcs::World World;
    NResources::ResourceHandle<NResources::MeshData> Mesh;
    NResources::ResourceHandle<NGraphics::Material> Material;

    Scene() {
        Mesh = Resources.Request<NResources::MeshData>({"mesh", "triangle"});
        Resources.PublishReady(Resources.BeginLoading(Mesh),
                               std::make_shared<NResources::MeshData>(
                                       std::vector<NResources::MeshVertex>{{.Position = {-0.7F, -0.6F, 0.0F}},
                                                                           {.Position = {0.7F, -0.6F, 0.0F}},
                                                                           {.Position = {0.0F, 0.7F, 0.0F}}},
                                       std::vector<std::uint32_t>{0, 1, 2},
                                       std::vector<std::uint8_t>(3)));
        Material = Resources.Request<NGraphics::Material>({"material", "triangle"});
        Resources.PublishReady(
                Resources.BeginLoading(Material),
                std::make_shared<NGraphics::Material>(NResources::ResourceIdentity{"pipeline", "triangle"}));
        const auto camera = World.CreateEntity();
        World.AddComponent<NRenderer::TransformComponent>(camera);
        World.AddComponent<NRenderer::CameraComponent>(camera);
        const auto triangle = World.CreateEntity();
        World.AddComponent<NRenderer::TransformComponent>(triangle);
        World.AddComponent<NRenderer::RenderableComponent>(triangle, NRenderer::RenderableComponent{Mesh, Material});
    }
    NGraphics::GraphicsPipelineDescriptor Pipeline(NGraphics::EPixelFormat format) {
        auto shader = [&](NResources::EShaderStage stage, const std::vector<std::uint32_t>& words, const char* name) {
            const auto handle = Resources.Request<NResources::ShaderArtifact>({"shader", name});
            Resources.PublishReady(Resources.BeginLoading(handle),
                                   std::make_shared<NResources::ShaderArtifact>(stage, words));
            return NGraphics::Shader{Resources, handle};
        };
        NGraphics::GraphicsPipelineDescriptor desc;
        desc.VertexPushConstantBytes = 64;
        desc.Shaders = {shader(NResources::EShaderStage::VERTEX, VERT_WORDS, "triangle.vert"),
                        shader(NResources::EShaderStage::FRAGMENT, FRAG_WORDS, "triangle.frag")};
        desc.VertexLayout.Bindings = {{0, sizeof(NResources::MeshVertex), NGraphics::EVertexInputRate::VERTEX}};
        desc.VertexLayout.Attributes = {{0, 0, NGraphics::EVertexFormat::FLOAT32_3, 0}};
        desc.RasterState.CullMode = NGraphics::ECullMode::NONE;
        desc.ColorAttachmentFormats = {format};
        desc.ColorBlendAttachments.resize(1);
        return desc;
    }
};

// One Engine owns admission, frame storage and CPU execution. The application
// thread pumps WSI; workers publish only immutable plans and completion promises.
class Runtime final: public NEngine::NRuntime::IFrameRuntime {
public:
    Runtime(Scene& scene, NRenderer::GraphPresentation& presentation)
        : m_scene(scene)
        , m_presentation(presentation) {
    }
    void Update(const NEngine::NRuntime::FrameContext&) override {
    }
    void Draw(const NEngine::NRuntime::FrameContext& frame) override {
        const auto& snapshot =
                NRenderer::NInternal::ExtractRenderWorld(m_scene.World, m_scene.Resources, frame.GetStorage());
        const auto& plan = NRenderer::Renderer::Prepare(snapshot, frame.GetMemoryResource());
        auto pending = std::make_unique<Pending>();
        pending->Plan = &plan;
        frame.RegisterGpuCompletion(pending->Done.get_future().share());
        std::lock_guard lock{m_mutex};
        m_queued.emplace(plan.GetFrame().ApplicationFrameIndex, std::move(pending));
    }
    void Pump() {
        std::lock_guard lock{m_mutex};
        for (auto it = m_submitted.begin(); it != m_submitted.end();) {
            if (m_presentation.IsCompleted((*it)->Completion)) {
                (*it)->Done.set_value();
                it = m_submitted.erase(it);
                ++Completed;
            } else
                ++it;
        }
        if (m_queued.empty() || m_queued.begin()->first != m_next)
            return;
        auto& pending = *m_queued.begin()->second;
        m_submitted.emplace_back();
        NGraphics::FrameSubmissionResult result;
        try {
            result = m_presentation.Render(pending.Plan->GetFrame(), [&](const auto& target) {
                NRenderer::RenderGraphBuilder builder{pending.Plan->GetFrame()};
                const auto color = builder.ImportSwapchain(target, NRenderer::EResourceUsage::UNDEFINED);
                NRenderer::GraphicsPassDescription pass;
                pass.Writes = {{color, NRenderer::EResourceUsage::COLOR_ATTACHMENT_WRITE}};
                pass.ColorAttachments = {{.Resource = color, .ClearColor = {0.02F, 0.04F, 0.08F, 1.0F}}};
                pass.DrawCommands.assign(pending.Plan->GetDrawCommands().begin(),
                                         pending.Plan->GetDrawCommands().end());
                if (pass.DrawCommands.size() != 1)
                    throw std::runtime_error{"Missing World/Renderer triangle"};
                (void)builder.AddGraphicsPass(std::move(pass));
                return builder.Compile();
            });
        } catch (...) {
            m_submitted.pop_back();
            throw;
        }
        if (result.Completion.IsValid()) {
            pending.Completion = result.Completion;
            m_submitted.back() = std::move(m_queued.begin()->second);
            m_queued.erase(m_queued.begin());
            ++m_next;
            ++Submitted;
        } else {
            m_submitted.pop_back();
        }
        if (result.Failure)
            std::rethrow_exception(result.Failure);
    }
    void Drain() noexcept override {
        try {
            // Engine has joined CPU producers; unsubmitted plans have no GPU consumers.
            {
                std::lock_guard lock{m_mutex};
                for (auto& [id, pending]: m_queued) {
                    (void)id;
                    pending->Done.set_value();
                }
                m_queued.clear();
            }
            while (!m_submitted.empty()) {
                Pump();
                std::this_thread::yield();
            }
        } catch (...) {
            std::terminate();
        }
    }
    std::uint64_t Submitted = 0;
    std::uint64_t Completed = 0;

private:
    struct Pending {
        const NRenderer::RenderPlan* Plan = nullptr;
        NGraphics::CompletionPoint Completion;
        std::promise<void> Done;
    };
    Scene& m_scene;
    NRenderer::GraphPresentation& m_presentation;
    std::mutex m_mutex;
    std::map<std::uint64_t, std::unique_ptr<Pending>> m_queued;
    std::deque<std::unique_ptr<Pending>> m_submitted;
    std::uint64_t m_next = 0;
};

struct Result {
    std::uint64_t Submitted, Completed, Recycled;
};
inline Result Run(std::size_t activeFrames,
                  std::size_t workers,
                  std::uint64_t frames,
                  bool exerciseResize,
                  const NGraphics::PresentationConfig& validation = {}) {
    NWindow::WindowRuntime windowRuntime;
    NWindow::WindowConfig config{NWindow::EWindowType::GLFW};
    config.Title = "World → Renderer → RenderGraph → Vulkan";
    config.Size = {480, 360};
    NWindow::Window window{config};
    Scene scene;
    auto presentationConfig = validation;
    presentationConfig.MaxActiveFrames = activeFrames;
    NRenderer::GraphPresentation presentation{window, presentationConfig};
    presentation.PrepareMesh(std::move(*scene.Resources.TryAcquire(scene.Mesh)));
    presentation.PrepareMaterial(std::move(*scene.Resources.TryAcquire(scene.Material)),
                                 scene.Pipeline(presentation.GetColorFormat()));
    auto runtime = std::make_unique<Runtime>(scene, presentation);
    auto* observer = runtime.get();
    auto engine = NEngine::NRuntime::EngineFactory::Create({activeFrames, workers}, std::move(runtime));
    engine->Start();
    std::uint64_t admitted = 0;
    bool update = true;
    bool resized = false;
    bool suspended = false;
    const auto start = std::chrono::steady_clock::now();
    try {
        while (!window.ShouldClose() && observer->Submitted < frames) {
            window.ProcessEvents();
            const auto size = window.GetFramebufferSize();
            presentation.SetPresentationSize(size.Width, size.Height);
            if (exerciseResize && observer->Submitted >= 4 && !resized) {
                window.SetSize({640, 400});
                resized = true;
            }
            if (exerciseResize && observer->Submitted >= 8 && !suspended) {
                presentation.SetPresentationSize(0, 0);
                const auto before = observer->Submitted;
                observer->Pump();
                if (observer->Submitted != before)
                    throw std::runtime_error{"Suspended graph was submitted"};
                presentation.SetPresentationSize(size.Width, size.Height);
                suspended = true;
            }
            observer->Pump();
            if (update) {
                if (admitted < frames && engine->Update()) {
                    ++admitted;
                    update = false;
                }
            } else if (engine->Draw())
                update = true;
            if (auto error = engine->GetLastError())
                throw std::runtime_error{error->Message};
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds{20})
                throw std::runtime_error{"Triangle timed out"};
            engine->WaitForProgress(engine->GetProgressVersion(), std::chrono::milliseconds{1});
        }
    } catch (...) {
        engine->Stop();
        throw;
    }
    engine->Stop();
    return {observer->Submitted, observer->Completed, engine->GetGpuRecycledFrameCount()};
}
} // namespace NTriangle
