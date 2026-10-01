#include <cstdint>
#include <memory>
#include <vector>

#include <GraphicsEngine/application/application_config.h>
#include <GraphicsEngine/ecs/world.h>
#include <GraphicsEngine/graphics/buffer.h>
#include <GraphicsEngine/graphics/graphics_capabilities.h>
#include <GraphicsEngine/graphics/graphics_pipeline.h>
#include <GraphicsEngine/graphics/shader.h>
#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/math/transform.h>
#include <GraphicsEngine/resources/resource_manager.h>
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
    const auto makeShader = [&resources](NResources::EShaderStage stage, const char* key) {
        const auto handle =
                resources.Request<NResources::ShaderArtifact>(NResources::ResourceIdentity{"shader", key});
        const auto operation = resources.BeginLoading(handle);
        resources.PublishReady(
                operation,
                std::make_shared<NResources::ShaderArtifact>(
                        stage,
                        std::vector<std::uint32_t>{0x07230203U, 0x00010000U, 0U, 2U, 0U}));
        return NGraphics::Shader{resources, handle};
    };
    const NGraphics::GraphicsPipelineDescriptor pipeline{
            .Shaders =
                    {
                            makeShader(NResources::EShaderStage::VERTEX, "package.vertex"),
                            makeShader(NResources::EShaderStage::FRAGMENT, "package.fragment"),
                    },
            .ColorAttachmentFormats = {NGraphics::EPixelFormat::BGRA8_SRGB},
            .ColorBlendAttachments = {NGraphics::BlendAttachmentDescriptor{}},
    };

    NGraphics::ValidateGraphicsPipelineDescriptor(pipeline);
    const std::uint64_t pipelineHash = NGraphics::HashGraphicsPipelineDescriptor(pipeline);

    if (!error || !supportsRequiredGraphics || buffer.SizeBytes != 256 || pipelineHash == 0) {
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

    return config.Window.Size.Width == 640 && config.Window.Size.Height == 480 && origin.W == 1.0F &&
                           world.IsAlive(entity) && artifact && validSpirV &&
                           artifact->GetStage() == NResources::EShaderStage::VERTEX
                 ? 0
                 : 1;
}
