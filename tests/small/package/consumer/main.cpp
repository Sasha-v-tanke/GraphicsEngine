#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <GraphicsEngine/application/application_config.h>
#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/graphics/buffer.h>
#include <GraphicsEngine/graphics/graphics_capabilities.h>
#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/resource_use_record.h>
#include <GraphicsEngine/math/transform.h>
#include <GraphicsEngine/resources/image_data.h>
#include <GraphicsEngine/resources/image_loader.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <GraphicsEngine/resources/resource_use_record.h>
#include <GraphicsEngine/resources/shader_artifact_loader.h>
#include <GraphicsEngine/window/window_runtime.h>
#include <GraphicsEngine/window/window_type.h>

int main() {
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

    if (!error || !supportsRequiredGraphics || buffer.SizeBytes != 256) {
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

    return config.Window.Size.Width == 640 && config.Window.Size.Height == 480 && origin.W == 1.0F &&
                           world.IsAlive(entity) && artifact && validSpirV &&
                           artifact->GetStage() == NResources::EShaderStage::VERTEX && image.GetWidth() == 1U &&
                           image.GetPixels().size() == 4U && loadedImageData && loadedImageData->GetWidth() == 1U
                 ? 0
                 : 1;
}
