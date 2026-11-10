#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

#include <application/runtime/scene_runtime.h>
#include <resources/async_resource_loader.h>
#include <samples/sample_7_model/scenario.h>

namespace NSceneSwitching {

[[nodiscard]] inline const char* StateName(NResources::EResourceState state) {
    switch (state) {
    case NResources::EResourceState::UNLOADED:
        return "UNLOADED";
    case NResources::EResourceState::LOADING:
        return "LOADING";
    case NResources::EResourceState::READY:
        return "READY";
    case NResources::EResourceState::FAILED:
        return "FAILED";
    case NResources::EResourceState::UNLOADING:
        return "UNLOADING";
    case NResources::EResourceState::QUEUED:
        return "QUEUED";
    case NResources::EResourceState::CPU_READY:
        return "CPU_READY";
    case NResources::EResourceState::UPLOADING:
        return "UPLOADING";
    }
    return "UNKNOWN";
}

// A controllably slow filesystem backend, used only on ResourceIo's dedicated threads.
// Releasing after rendered frames makes the loading/skip path reproducible even on fast disks.
struct DelayedFile {
    std::mutex Mutex;
    std::condition_variable_any Wake;
    bool Released = false;

    NResources::ResourceIoBytes Read(const std::filesystem::path& path, std::stop_token stop, std::size_t limit) {
        {
            std::unique_lock lock{Mutex};
            Wake.wait(lock, stop, [&] { return Released; });
        }
        if (stop.stop_requested()) {
            return {};
        }
        std::ifstream file{path, std::ios::binary | std::ios::ate};
        const auto size = file.tellg();
        if (!file || size < 0 || static_cast<std::uint64_t>(size) > limit) {
            throw std::runtime_error{"Cannot read bounded background mesh"};
        }
        NResources::ResourceIoBytes bytes(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            throw std::runtime_error{"Incomplete background mesh read"};
        }
        return bytes;
    }
    void Release() {
        std::lock_guard lock{Mutex};
        Released = true;
        Wake.notify_all();
    }
};

struct Result {
    std::uint64_t Submitted = 0;
    std::uint64_t Completed = 0;
    std::uint64_t Recycled = 0;
    std::uint64_t LoadingFrames = 0;
    std::uint64_t ReadyFrames = 0;
    std::uint64_t EmptyFrames = 0;
    std::uint64_t Publications = 0;
};

// Workers publish immutable frame data; only the owner pumps presentation and native asset preparation.
class Presentation final {
public:
    explicit Presentation(NRenderer::GraphPresentation& presentation)
        : m_presentation(presentation) {
    }

    void Queue(const NEngine::NRuntime::FrameContext& frame,
               const NApplication::NRuntime::ScenePublication& publication,
               const NRenderer::RenderWorld& snapshot,
               const NRenderer::RenderPlan& plan) {
        auto pending = std::make_unique<Pending>();
        pending->Publication = publication;
        pending->Snapshot = &snapshot;
        pending->Plan = &plan;
        frame.RegisterGpuCompletion(pending->Done.get_future().share());
        std::lock_guard lock{m_mutex};
        m_queued.emplace(publication.Frame.GetFrameSequence(), std::move(pending));
    }

    void Pump() {
        std::lock_guard lock{m_mutex};
        while (!m_submitted.empty() && m_presentation.IsCompleted(m_submitted.front()->Completion)) {
            m_submitted.front()->Done.set_value();
            m_submitted.pop_front();
            ++Diagnostics.Completed;
        }
        const auto it = m_queued.find(m_next);
        if (it == m_queued.end()) {
            return;
        }
        auto& pending = *it->second;
        // Reserve retention before a native submit can succeed.
        m_submitted.emplace_back();
        NGraphics::FrameSubmissionResult result;
        try {
            result = m_presentation.Render(pending.Plan->GetFrame(), [&](const auto& target) {
                NRenderer::RenderGraphBuilder builder{pending.Plan->GetFrame()};
                const auto color = builder.ImportSwapchain(target, NRenderer::EResourceUsage::UNDEFINED);
                NRenderer::GraphicsPassDescription pass;
                pass.Writes = {{color, NRenderer::EResourceUsage::COLOR_ATTACHMENT_WRITE}};
                const bool first = pending.Publication.Name == "harbor";
                pass.ColorAttachments = {
                        {.Resource = color, .ClearColor = {0.02F, first ? 0.04F : 0.12F, 0.08F, 1.0F}}};
                pass.DrawCommands.assign(pending.Plan->GetDrawCommands().begin(),
                                         pending.Plan->GetDrawCommands().end());
                (void)builder.AddGraphicsPass(std::move(pass));
                return builder.Compile();
            });
        } catch (...) {
            m_submitted.pop_back();
            throw;
        }
        if (result.Completion.IsValid()) {
            if (!pending.Publication.Scene) {
                ++Diagnostics.EmptyFrames;
            } else if (!pending.Snapshot->GetResourceDiagnostics().empty()) {
                ++Diagnostics.LoadingFrames;
            } else {
                ++Diagnostics.ReadyFrames;
            }
            if (Verbose) {
                std::cout << "publication frame=" << pending.Publication.Frame.GetFrameSequence()
                          << " slot=" << pending.Publication.Frame.GetSlotIndex()
                          << " scene=" << (pending.Publication.Scene ? pending.Publication.Name : "<empty>")
                          << " draws=" << pending.Plan->GetDrawCommands().size();
                for (const auto& diagnostic: pending.Snapshot->GetResourceDiagnostics()) {
                    std::cout << " resource=" << diagnostic.Source.Identity->GetKey()
                              << " state=" << StateName(diagnostic.Source.State) << " policy=skip";
                }
                std::cout << '\n';
            }
            pending.Completion = result.Completion;
            m_submitted.back() = std::move(it->second);
            m_queued.erase(it);
            ++m_next;
            ++Diagnostics.Submitted;
        } else {
            m_submitted.pop_back();
        }
        if (result.Failure) {
            std::rethrow_exception(result.Failure);
        }
    }

    void Drain() noexcept {
        try {
            {
                std::lock_guard lock{m_mutex};
                for (auto& [_, pending]: m_queued) {
                    pending->Done.set_value();
                }
                m_queued.clear();
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (!m_submitted.empty()) {
                Pump();
                if (std::chrono::steady_clock::now() >= deadline) {
                    std::terminate();
                }
                std::this_thread::yield();
            }
        } catch (...) {
            std::terminate();
        }
    }

    Result Diagnostics;
    bool Verbose = false;

private:
    struct Pending {
        NApplication::NRuntime::ScenePublication Publication;
        const NRenderer::RenderWorld* Snapshot = nullptr;
        const NRenderer::RenderPlan* Plan = nullptr;
        NGraphics::CompletionPoint Completion;
        std::promise<void> Done;
    };
    NRenderer::GraphPresentation& m_presentation;
    std::mutex m_mutex;
    std::map<std::uint64_t, std::unique_ptr<Pending>> m_queued;
    std::deque<std::unique_ptr<Pending>> m_submitted;
    std::uint64_t m_next = 0;
};

inline Result Run(std::size_t activeFrames,
                  std::size_t workers,
                  std::uint64_t frames,
                  bool verbose = false,
                  const NGraphics::PresentationConfig& validation = {}) {
    NWindow::WindowRuntime windowRuntime;
    NWindow::WindowConfig config{NWindow::EWindowType::GLFW};
    config.Title = "Scene switching / shared resources";
    config.Size = {480, 360};
    NWindow::Window window{config};
    // Reuse sample 7's asset bootstrap and shaders. Both runtime scenes refer to this external manager.
    NModel::Scene assets;
    auto presentationConfig = validation;
    presentationConfig.MaxActiveFrames = activeFrames;
    NRenderer::GraphPresentation presentation{window, presentationConfig};
    assets.Prepare(presentation);
    NCommon::TaskSystem resourceTasks{workers};
    DelayedFile file;
    NResources::AsyncResourceLoader loader{assets.Resources, resourceTasks, {}, [&](auto& path, auto stop, auto limit) {
                                               return file.Read(path, stop, limit);
                                           }};
    const auto delayed =
            loader.LoadMesh({"mesh", "background-cube"},
                            std::filesystem::path{__FILE__}.parent_path() / "../sample_7_model/assets/cube.obj");
    NScene::SceneManager scenes;
    const auto create = [&](const char* name, float angle) {
        const auto handle = scenes.CreateScene(name);
        auto& world = scenes.Acquire(handle)->GetWorld();
        const auto camera = world.CreateEntity();
        world.AddComponent<NRenderer::TransformComponent>(
                camera,
                NRenderer::TransformComponent{.Value = {.Translation = {0, 0, 4}}});
        world.AddComponent<NRenderer::CameraComponent>(
                camera,
                NRenderer::CameraComponent{.Projection = {1.04719755F, 4.0F / 3.0F, 0.1F, 100.0F}});
        float x = -0.8F;
        for (const auto mesh: {assets.Mesh, delayed}) {
            const auto object = world.CreateEntity();
            world.AddComponent<NRenderer::TransformComponent>(
                    object,
                    NRenderer::TransformComponent{.Value = {.Translation = {x, 0, 0},
                                                            .Rotation = NMath::AngleAxis(angle, {0, 1, 0}),
                                                            .Scale = {0.4F, 0.4F, 0.4F}}});
            world.AddComponent<NRenderer::RenderableComponent>(object,
                                                               NRenderer::RenderableComponent{mesh, assets.Material});
            x = 0.8F;
        }
        return handle;
    };
    auto harbor = create("harbor", 0.5F);
    const auto forest = create("forest", -0.5F);
    Presentation consumer{presentation};
    consumer.Verbose = verbose;
    auto runtime = std::make_unique<NApplication::NRuntime::SceneRuntime>(
            assets.Resources,
            [&](const auto& frame, const auto& publication, const auto& snapshot, const auto& plan) {
                consumer.Queue(frame, publication, snapshot, plan);
            },
            [&] { consumer.Drain(); });
    auto* host = runtime.get();
    auto engine = NEngine::NRuntime::EngineFactory::Create({activeFrames, workers}, std::move(runtime));
    engine->Start();
    std::uint64_t admitted = 0;
    bool prepared = false;
    const auto start = std::chrono::steady_clock::now();
    try {
        while (!window.ShouldClose() && consumer.Diagnostics.Submitted < frames) {
            const auto progress = engine->GetProgressVersion();
            window.ProcessEvents();
            const auto size = window.GetFramebufferSize();
            presentation.SetPresentationSize(size.Width, size.Height);
            consumer.Pump();
            if (consumer.Diagnostics.Submitted >= 8) {
                file.Release();
            }
            loader.PumpUploads();
            if (!prepared) {
                if (auto lease = assets.Resources.TryAcquire(delayed)) {
                    // This backend uses host-visible mesh buffers: preparation performs no file I/O or GPU wait.
                    presentation.PrepareMesh(std::move(*lease));
                    prepared = true;
                } else if (assets.Resources.GetState(delayed) == NResources::EResourceState::FAILED) {
                    throw std::runtime_error{assets.Resources.GetFailure(delayed)->Message};
                }
            }
            if (admitted < frames) {
                const auto frame = host->TryBeginFrame(*engine, scenes, [&](auto& registry) {
                    // Every publication occurs after old World readers joined, while GPU slots can remain live.
                    if (!registry.IsRegistered(harbor)) {
                        harbor = create("harbor", 0.5F);
                    }
                    registry.Activate(admitted % 2 == 0 ? harbor : forest);
                    if (admitted % 8 == 6) {
                        registry.Activate(harbor);
                        registry.Unload(harbor); // Defined empty state, rendered as a clear-only graph.
                    }
                    if (!assets.Resources.TryAcquire(assets.Mesh) || !assets.Resources.TryAcquire(assets.Image)) {
                        throw std::runtime_error{"Scene unload invalidated shared assets"};
                    }
                });
                if (frame) {
                    ++admitted;
                    ++consumer.Diagnostics.Publications;
                    if (!engine->CompleteUserUpdate(*frame) || !engine->Draw()) {
                        throw std::runtime_error{"Scene frame handoff rejected"};
                    }
                }
            }
            if (const auto error = engine->GetLastError()) {
                throw std::runtime_error{error->Message};
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds{20}) {
                throw std::runtime_error{"Scene switching timed out"};
            }
            engine->WaitForProgress(progress, std::chrono::milliseconds{1});
        }
    } catch (...) {
        loader.Shutdown();
        engine->Stop();
        throw;
    }
    loader.Shutdown();
    engine->Stop();
    auto result = consumer.Diagnostics;
    result.Recycled = engine->GetGpuRecycledFrameCount();
    return result;
}

} // namespace NSceneSwitching
