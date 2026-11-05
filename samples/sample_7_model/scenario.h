#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <stdexcept>

#include <resources/image_loader.h>
#include <resources/mesh_loader.h>
#include <samples/sample_5_triangle/scenario.h>
#include <samples/sample_6_textured/shaders.h>

namespace NModel {
struct Scene {
    NResources::ResourceManager Resources;
    NEcs::World World;
    NResources::ResourceHandle<NResources::MeshData> Mesh;
    NResources::ResourceHandle<NGraphics::Material> Material;
    NResources::ResourceHandle<NResources::ImageData> Image;
    NResources::ResourceHandle<NGraphics::SamplerDescriptor> Sampler;

    NEcs::Entity Model;
    NEcs::Entity Camera;
    std::atomic<float> AspectRatio{4.0F / 3.0F};

    explicit Scene(const std::filesystem::path& directory = std::filesystem::path{__FILE__}.parent_path() / "assets") {
        Mesh = NResources::LoadMesh(Resources, {"mesh", "cube"}, directory / "cube.obj");
        Image = NResources::LoadImage(Resources, {"image", "checker"}, directory / "checker.png");
        if (!Resources.TryAcquire(Mesh)) {
            throw std::runtime_error{Resources.GetFailure(Mesh)->Message};
        }
        if (!Resources.TryAcquire(Image)) {
            throw std::runtime_error{Resources.GetFailure(Image)->Message};
        }
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
        Camera = World.CreateEntity();
        World.AddComponent<NRenderer::TransformComponent>(
                Camera,
                NRenderer::TransformComponent{.Value = {.Translation = {0, 0, 4}}});
        World.AddComponent<NRenderer::CameraComponent>(
                Camera,
                NRenderer::CameraComponent{.Projection = {1.04719755F, AspectRatio.load(), 0.1F, 100.0F}});
        Model = World.CreateEntity();
        World.AddComponent<NRenderer::TransformComponent>(Model);
        World.AddComponent<NRenderer::RenderableComponent>(Model, NRenderer::RenderableComponent{Mesh, Material});
        Update(0);
    }

    // Only publish scalar window data from main. The worker owns component mutation.
    void SetViewport(int width, int height) {
        if (width > 0 && height > 0) {
            AspectRatio.store(static_cast<float>(width) / static_cast<float>(height));
        }
    }

    void Update(std::uint64_t frame) {
        World.GetComponent<NRenderer::TransformComponent>(Model).Value.Rotation =
                NMath::AngleAxis(0.5F + static_cast<float>(frame) * 0.01F, {0, 1, 0}) *
                NMath::AngleAxis(-0.35F, {1, 0, 0});
        World.GetComponent<NRenderer::CameraComponent>(Camera).Projection.AspectRatio = AspectRatio.load();
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
        desc.Shaders = {shader(NResources::EShaderStage::VERTEX, NTextured::VERT_WORDS, "textured.vert"),
                        shader(NResources::EShaderStage::FRAGMENT, NTextured::FRAG_WORDS, "textured.frag")};
        desc.VertexLayout.Bindings = {{0, sizeof(NResources::MeshVertex), NGraphics::EVertexInputRate::VERTEX}};
        desc.VertexLayout.Attributes = {
                {0, 0, NGraphics::EVertexFormat::FLOAT32_3, 0},
                {1, 0, NGraphics::EVertexFormat::FLOAT32_2, offsetof(NResources::MeshVertex, Texcoord)}};
        // A convex closed mesh with back-face culling needs no depth attachment.
        desc.RasterState.CullMode = NGraphics::ECullMode::BACK;
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
} // namespace NModel
