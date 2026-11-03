#pragma once

#include <cstddef>

#include <samples/sample_5_triangle/scenario.h>
#include <samples/sample_6_textured/shaders.h>

namespace NTextured {
struct Scene {
    NResources::ResourceManager Resources;
    NEcs::World World;
    NResources::ResourceHandle<NResources::MeshData> Mesh;
    NResources::ResourceHandle<NGraphics::Material> Material;
    NResources::ResourceHandle<NResources::ImageData> Image;
    NResources::ResourceHandle<NGraphics::SamplerDescriptor> Sampler;

    Scene() {
        Mesh = Resources.Request<NResources::MeshData>({"mesh", "textured"});
        Resources.PublishReady(
                Resources.BeginLoading(Mesh),
                std::make_shared<NResources::MeshData>(
                        std::vector<NResources::MeshVertex>{{.Position = {-0.8F, -0.7F, 0}, .Texcoord = {0, 0}},
                                                            {.Position = {0.8F, -0.7F, 0}, .Texcoord = {1, 0}},
                                                            {.Position = {0, 0.8F, 0}, .Texcoord = {0.5F, 1}}},
                        std::vector<std::uint32_t>{0, 1, 2},
                        std::vector<std::uint8_t>(3, NResources::MeshData::TEXCOORD_PRESENT)));
        Image = Resources.Request<NResources::ImageData>({"image", "checker"});
        Resources.PublishReady(Resources.BeginLoading(Image),
                               std::make_shared<NResources::ImageData>(2,
                                                                       2,
                                                                       NResources::EImagePixelFormat::RGBA8,
                                                                       std::vector<std::uint8_t>{255,
                                                                                                 40,
                                                                                                 20,
                                                                                                 255,
                                                                                                 20,
                                                                                                 240,
                                                                                                 40,
                                                                                                 255,
                                                                                                 20,
                                                                                                 40,
                                                                                                 255,
                                                                                                 255,
                                                                                                 250,
                                                                                                 230,
                                                                                                 20,
                                                                                                 255}));
        Sampler = Resources.Request<NGraphics::SamplerDescriptor>({"sampler", "nearest"});
        Resources.PublishReady(Resources.BeginLoading(Sampler),
                               std::make_shared<NGraphics::SamplerDescriptor>(
                                       NGraphics::SamplerDescriptor{.MinFilter = NGraphics::ESamplerFilter::Nearest,
                                                                    .MagFilter = NGraphics::ESamplerFilter::Nearest}));
        Material = Resources.Request<NGraphics::Material>({"material", "textured"});
        Resources.PublishReady(Resources.BeginLoading(Material),
                               std::make_shared<NGraphics::Material>(
                                       NResources::ResourceIdentity{"pipeline", "textured"},
                                       std::vector<NGraphics::MaterialBindingLayoutEntry>{
                                               {0,
                                                NGraphics::EMaterialBindingType::CombinedImageSampler,
                                                NGraphics::ShaderVisibility(NGraphics::EShaderVisibility::Fragment)}},
                                       std::vector<NGraphics::MaterialBinding>{
                                               {.Binding = 0,
                                                .Type = NGraphics::EMaterialBindingType::CombinedImageSampler,
                                                .Buffer = {},
                                                .Image = {},
                                                .Sampler = {},
                                                .CombinedImageSampler = {Resources.GetIdentity(Image),
                                                                         Resources.GetIdentity(Sampler)}}}));
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
        desc.Shaders = {shader(NResources::EShaderStage::VERTEX, VERT_WORDS, "textured.vert"),
                        shader(NResources::EShaderStage::FRAGMENT, FRAG_WORDS, "textured.frag")};
        desc.VertexLayout.Bindings = {{0, sizeof(NResources::MeshVertex), NGraphics::EVertexInputRate::VERTEX}};
        desc.VertexLayout.Attributes = {
                {0, 0, NGraphics::EVertexFormat::FLOAT32_3, 0},
                {1, 0, NGraphics::EVertexFormat::FLOAT32_2, offsetof(NResources::MeshVertex, Texcoord)}};
        desc.RasterState.CullMode = NGraphics::ECullMode::NONE;
        desc.ColorAttachmentFormats = {format};
        desc.ColorBlendAttachments.resize(1);
        return desc;
    }

    void Prepare(NRenderer::GraphPresentation& presentation) {
        presentation.PrepareImage(Resources, Image);
        presentation.PrepareSampler(Resources, Sampler);
        presentation.PrepareMesh(std::move(*Resources.TryAcquire(Mesh)));
        presentation.PrepareMaterial(std::move(*Resources.TryAcquire(Material)),
                                     Pipeline(presentation.GetColorFormat()));
    }
};
} // namespace NTextured
