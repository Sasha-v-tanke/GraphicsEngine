#include <memory>
#include <string>

#include <GraphicsEngine/application/application_config.h>
#include <GraphicsEngine/lib/common/error/error.h>
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

    if (!error) {
        return 1;
    }

    const auto shader = resources.Request<ShaderArtifact>(NResources::ResourceIdentity{"shader", "package.spv"});
    const auto operation = resources.BeginLoading(shader);

    resources.PublishReady(operation, std::make_shared<ShaderArtifact>(ShaderArtifact{.Name = "package"}));

    const std::shared_ptr<const ShaderArtifact> artifact = resources.GetCpuResource(shader);

    return config.Window.Size.Width == 640 && config.Window.Size.Height == 480 && artifact &&
                           artifact->Name == "package"
                 ? 0
                 : 1;
}
