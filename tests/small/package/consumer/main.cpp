#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <utility>
#include <vector>

#include <GraphicsEngine/application/application.h>
#include <GraphicsEngine/application/application_config.h>
#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/graphics/buffer.h>
#include <GraphicsEngine/graphics/graphics_capabilities.h>
#include <GraphicsEngine/graphics/image.h>
#include <GraphicsEngine/graphics/material.h>
#include <GraphicsEngine/graphics/pipeline_key.h>
#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/resource_use_record.h>
#include <GraphicsEngine/lib/thread/job.h>
#include <GraphicsEngine/lib/thread/task/task_system.h>
#include <GraphicsEngine/math/transform.h>
#include <GraphicsEngine/renderer/render_graph.h>
#include <GraphicsEngine/renderer/renderer.h>
#include <GraphicsEngine/resources/image_data.h>
#include <GraphicsEngine/resources/image_loader.h>
#include <GraphicsEngine/resources/mesh_loader.h>
#include <GraphicsEngine/resources/resource_io.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <GraphicsEngine/resources/resource_use_record.h>
#include <GraphicsEngine/resources/shader_artifact_loader.h>
#include <GraphicsEngine/scene/scene_manager.h>
#include <GraphicsEngine/window/window_runtime.h>
#include <GraphicsEngine/window/window_type.h>

// Instantiates the facade using only installed public headers.
class UserJobsConsumer: public NApplication::Application {
public:
    using Application::Application;
    void MainAction() {
        RunOnMainThread([value = std::make_unique<int>(1)] { (void)value; });
    }
    NCommon::Job<std::unique_ptr<int>> StartJob() {
        return RunAsync([value = std::make_unique<int>(1)](std::stop_token) mutable { return std::move(value); });
    }
};

int main() {
    NRenderer::RenderGraphBuilder graphBuilder{{.Generation = 1}};
    const NRenderer::CompiledGraph graph = graphBuilder.Compile();
    graph.ValidateResources({.Generation = 1});
    NScene::SceneManager scenes;
    const auto scene = scenes.CreateScene("package consumer");
    scenes.Activate(scene);
    if (!scenes.AcquireActive() || scenes.GetActiveHandle() != scene) {
        return 1;
    }
    scenes.Unload(scene);
    NWindow::WindowRuntime runtime;
    NResources::ResourceManager resources;

    NApplication::ApplicationConfig config{
            .Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
    };

    config.Window.Size = {
            .Width = 640,
            .Height = 480,
    };

    const std::error_code error = NCommon::make_error_code(NCommon::EError::INVALID_STATE);
    NEcs::World world;
    const NEcs::Entity entity = world.CreateEntity();
    NCommon::TaskSystem ecsTasks{2};
    world.RegisterSystem({}, [](NEcs::World&) {}, NEcs::SystemSchedule{.Id = 1});
    world.RunSystems(ecsTasks);
    const bool supportsRequiredGraphics = NGraphics::SatisfiesRequirements(
            {
                    .Presentation = true,
                    .TimelineCompletion = true,
                    .MaxFramesInFlight = 2,
            },
            {
                    .Presentation = true,
                    .TimelineCompletion = false,
                    .MaxFramesInFlight = 1,
            });
    const NMath::Vec4 origin = NMath::ComposeTransform({}) * NMath::Vec4{.W = 1.0F};
    const NGraphics::BufferDescriptor buffer{
            .SizeBytes = 256,
            .Usage = NGraphics::BufferUsage(NGraphics::EBufferUsage::Uniform),
            .Access = NGraphics::BufferAccess(NGraphics::EBufferAccess::GpuRead),
            .Lifetime = NGraphics::EBufferLifetime::Persistent,
    };
    const NGraphics::ImageDescriptor gpuImage{
            .Extent = {.Width = 1, .Height = 1, .Depth = 1},
            .Format = NGraphics::EImageFormat::RGBA8_UNORM,
            .Usage = NGraphics::ImageUsage(NGraphics::EImageUsage::Sampled),
            .Access = NGraphics::ImageAccess(NGraphics::EImageAccess::GpuRead),
            .Lifetime = NGraphics::EImageLifetime::Persistent,
    };
    const NGraphics::ImageViewDescriptor gpuImageView{
            .Format = NGraphics::EImageFormat::RGBA8_UNORM,
            .Aspects = NGraphics::ImageAspect(NGraphics::EImageAspect::Color),
    };
    const NGraphics::SamplerDescriptor gpuSampler{
            .MinFilter = NGraphics::ESamplerFilter::Linear,
            .MagFilter = NGraphics::ESamplerFilter::Linear,
            .AddressModeU = NGraphics::ESamplerAddressMode::ClampToEdge,
    };
    const NGraphics::Material material{NResources::ResourceIdentity{"pipeline", "package"}};

    if (!error || !supportsRequiredGraphics || buffer.SizeBytes != 256 || gpuImage.Extent.Width != 1 ||
        gpuImageView.Aspects == 0 || gpuSampler.AddressModeU != NGraphics::ESamplerAddressMode::ClampToEdge ||
        material.GetPipeline().GetKey() != "package") {
        return 1;
    }

    const auto shader =
            resources.Request<NResources::ShaderArtifact>(NResources::ResourceIdentity{"shader", "package.spv"});
    const auto operation = resources.BeginLoading(shader);
    const bool validSpirV = NResources::IsValidSpirVArtifact({0x07230203, 0x00010000, 0, 1, 0});

    resources.PublishReady(
            operation,
            std::make_shared<NResources::ShaderArtifact>(NResources::EShaderStage::VERTEX,
                                                         std::vector<std::uint32_t>{0x07230203, 0x00010000, 0, 1, 0}));

    const std::shared_ptr<const NResources::ShaderArtifact> artifact = resources.GetCpuResource(shader);
    NGraphics::GraphicsPipelineDescriptor pipelineDescriptor;
    pipelineDescriptor.Shaders.emplace_back(resources, shader);
    pipelineDescriptor.DepthAttachmentFormat = NGraphics::EPixelFormat::D32_FLOAT;
    const NGraphics::PipelineKey pipelineKey{pipelineDescriptor};
    if (pipelineKey != NGraphics::PipelineKey{pipelineDescriptor} ||
        pipelineKey.GetHash() != NGraphics::HashGraphicsPipelineDescriptor(pipelineDescriptor)) {
        return 1;
    }

    const NResources::ImageData image{
            1,
            1,
            NResources::EImagePixelFormat::RGBA8,
            std::vector<std::uint8_t>{255U, 0U, 0U, 255U},
    };
    const std::filesystem::path imagePath =
            std::filesystem::temp_directory_path() / "graphics-engine-package-image.ppm";
    {
        std::ofstream file{imagePath, std::ios::binary | std::ios::trunc};
        const std::vector<std::uint8_t> bytes =
                {'P', '6', '\n', '1', '\n', '1', '\n', '2', '5', '5', '\n', 255U, 0U, 0U};

        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    const auto loadedImage =
            NResources::LoadImage(resources, NResources::ResourceIdentity{"image", "package.ppm"}, imagePath);
    const std::shared_ptr<const NResources::ImageData> loadedImageData = resources.GetCpuResource(loadedImage);
    std::filesystem::remove(imagePath);

    const std::filesystem::path meshPath = std::filesystem::temp_directory_path() / "graphics-engine-package-mesh.obj";
    {
        std::ofstream file{meshPath};
        file << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    const auto loadedMesh =
            NResources::LoadMesh(resources, NResources::ResourceIdentity{"mesh", "package.obj"}, meshPath);
    const auto meshLease = resources.TryAcquire(loadedMesh);
    std::promise<std::shared_ptr<const NResources::ResourceIoResult>> ioDone;
    std::future<std::shared_ptr<const NResources::ResourceIoResult>> ioFuture = ioDone.get_future();
    NResources::ResourceIo io;
    const auto read = io.Read(meshPath, [&ioDone](std::shared_ptr<const NResources::ResourceIoResult> result) {
        ioDone.set_value(std::move(result));
    });
    const auto ioResult = ioFuture.get();
    io.Shutdown();
    if (!read.GetResult() || ioResult->Status != NResources::EResourceIoStatus::SUCCEEDED || !ioResult->Bytes ||
        ioResult->Bytes->empty()) {
        return 1;
    }
    std::filesystem::remove(meshPath);
    if (!meshLease || (*meshLease)->GetVertices().size() != 3 || (*meshLease)->GetIndices().size() != 3) {
        return 1;
    }

    return config.Window.Size.Width == 640 && config.Window.Size.Height == 480 && origin.W == 1.0F &&
                           world.IsAlive(entity) && artifact && validSpirV &&
                           artifact->GetStage() == NResources::EShaderStage::VERTEX && image.GetWidth() == 1U &&
                           image.GetPixels().size() == 4U && loadedImageData && loadedImageData->GetWidth() == 1U
                 ? 0
                 : 1;
}
