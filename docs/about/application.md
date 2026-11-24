# Application: игровой режим и сервисы

`Application` управляет окном и кадрами. Необязательный `ApplicationConfig::Game` подключает Scene/ECS к тому же
циклу выполнения. На текущем этапе Game mode обновляет ручную Scene и выводит очищенный кадр; автоматическая
подготовка Mesh/Material и отображение выбранной камеры добавляются следующими задачами интеграции.

```cpp
#include <GraphicsEngine/application/application.h>
#include <GraphicsEngine/renderer/render_components.h>

struct Settings {};

class Game final: public NApplication::Application {
public:
    Game()
        : Application({.Window = NWindow::WindowConfig{NWindow::EWindowType::GLFW},
                       .MaxActiveFrames = 2,
                       .WorkerCount = 2,
                       .Game = NApplication::GameConfig{}}) {
        GetServices().Register<Settings, NScene::SceneManager>();
        const auto scene = GetScenes().CreateScene("main");
        auto& world = GetScenes().Acquire(scene)->GetWorld();
        const auto camera = world.CreateEntity();
        world.AddComponent<NRenderer::TransformComponent>(camera);
        world.AddComponent<NRenderer::CameraComponent>(camera);
        GetScenes().Activate(scene);
        BindCamera(GetMainWindowView(), scene, world.GetEntityReference(camera));
    }
};

// On the application/main thread:
// Game game;
// game.Run();
```

## Конфигурация

- `Game = std::nullopt` — прежний CPU-only режим либо clear presentation, если задан Presentation.
- `GameConfig{.RenderingMode = ERenderingMode::DISABLED}` — тот же выбор.
- `GameConfig{}` / `STANDARD_3D` — явно включённая игровая композиция. Требует сборки с Vulkan и GLFW.
- `Presentation` сохраняет настройки validation; если не задан, используются значения по умолчанию.
- `MaxActiveFrames > 0` задаёт единую глубину кадров; значение PresentationConfig отдельно не увеличивает её.

## Доступ к Scene и сервисам

`GetServices()` возвращает ServiceManager приложения. SceneManager, ResourceManager и ViewRegistry уже существуют;
повторная регистрация этих типов отклоняется. `Register<T, Dependencies...>(args...)` добавляет owned сервис до
первого Run. Необязательные hooks: `void Start(const ServiceContext&)` и `void Stop()`; зависимости запускаются раньше,
останавливаются позже. Объекты сохраняются между Run. Полученные ссылки не продлевают lifetime Application.

`GetScenes()` и `GetResources()` возвращают соответствующие общие менеджеры. Пользователь вручную создаёт и
активирует Scene: существование Scene само по себе не включает её. `GetMainWindowView()` возвращает логическое
представление главного окна. `BindCamera(view, scene, entityReference)` вызывается до Run на owner thread;
foreign/stale identities отклоняются. Runtime scene switch и публикация выбранной камеры пока не предоставляются
этой композицией. Не изменяйте SceneManager/World из оконных callbacks параллельно с ECS.

`OnUpdate()` и `OnPostUpdate()` работают на worker; оконные callbacks — на основном потоке. Доступ к менеджеру
сервисов не отменяет ограничения потоков каждого сервиса. `RunOnMainThread()` выполняет короткое действие на
основном потоке во время Run. Для изменяемого World используйте разрешённую ECS-фазу, а не произвольную main-команду.

## Ошибки и завершение

Неверный mode или нулевая глубина кадров дают `INVALID_ARGUMENT`, недоступный backend — `UNSUPPORTED`.
Startup откатывает уже начатые сервисы; после чистого rollback допустима повторная попытка. Сервис, бросающий из
собственного Start, должен сам откатить частичную работу. Backend failure или неудачный Stop запрещают restart.

`RequestShutdown()` закрывает окно. Run присоединяет CPU/GPU consumers до остановки сервисов и возвращает первую
ошибку. После штатного завершения новый Run сохраняет объекты сервисов; окно должно оставаться пригодным для
следующего запуска. Объект приложения и поля наследника обязаны переживать работающий Run и его callbacks.

[Внутреннее устройство и проверки](../../application/README.md).
