#include <memory>
#include <string>

#include <GraphicsEngine/application/application_config.h>
#include <GraphicsEngine/graphics/buffer.h>
#include <GraphicsEngine/graphics/graphics_capabilities.h>
#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/math/transform.h>
#include <GraphicsEngine/resources/resource_manager.h>
#include <GraphicsEngine/window/window_runtime.h>
#include <GraphicsEngine/window/window_type.h>

struct ShaderArtifact {
    std::string Name;
};

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

    const auto shader = resources.Request<ShaderArtifact>(NResources::ResourceIdentity{"shader", "package.spv"});
    const auto operation = resources.BeginLoading(shader);

    resources.PublishReady(operation, std::make_shared<ShaderArtifact>(ShaderArtifact{.Name = "package"}));

    const std::shared_ptr<const ShaderArtifact> artifact = resources.GetCpuResource(shader);

    return config.Window.Size.Width == 640 && config.Window.Size.Height == 480 && origin.W == 1.0F && artifact &&
                           artifact->Name == "package"
                 ? 0
                 : 1;
}
