#include <memory>

#include <GraphicsEngine/application/application.h>
#include <GraphicsEngine/renderer/render_components.h>

namespace NPackageTest {
struct GameSettings {};

class InstalledGame final: public NApplication::Application {
public:
    InstalledGame()
        : Application(
                  {.Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW}, .Game = NApplication::GameConfig{}}) {
        GetServices().Register<GameSettings, NScene::SceneManager>();
        const auto scene = GetScenes().CreateScene("installed-game");
        auto& world = GetScenes().Acquire(scene)->GetWorld();
        const auto camera = world.CreateEntity();
        world.AddComponent<NRenderer::TransformComponent>(camera);
        world.AddComponent<NRenderer::CameraComponent>(camera);
        GetScenes().Activate(scene);
        BindCamera(GetMainWindowView(), scene, world.GetEntityReference(camera));
        (void)GetResources();
    }
};

// Compile/link the entire public Game setup without creating a window in the package test process.
std::unique_ptr<NApplication::Application> CreateInstalledGame() {
    return std::make_unique<InstalledGame>();
}
} // namespace NPackageTest
