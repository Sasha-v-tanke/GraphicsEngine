# Window

`Window` предоставляет framework-independent оконную абстракцию GraphicsEngine.

Публичный API модуля не зависит от GLFW, Qt, SDL, Vulkan, OpenGL или других
конкретных window/graphics API.

## Architecture

```text
User / Application
        |
        v
     Window
        |
        | owns
        v
  IWindowEngine
        ^
        |
        +---- GLFW
        |
        +---- Qt
        |
        +---- SDL
```

## Threading

`Window` имеет thread affinity.

Поток, на котором успешно завершился constructor `Window`, является его
owning thread.

Создание и уничтожение `Window` должны происходить на одном owning thread.

Следующие операции разрешено вызывать только на owning thread:

- `SetTitle()`;
- `SetSize()`;
- `GetSize()`;
- `GetFramebufferSize()`;
- `ShouldClose()`;
- `RequestClose()`;
- `ProcessEvents()`.

Window callbacks:

- `OnResize()`;
- `OnFramebufferResize()`;
- `OnClose()`;

также всегда вызываются на owning thread.

`IWindowEngine` и concrete framework implementation имеют ту же thread affinity,
что и владеющий ими `Window`.

Concrete implementation не переносит window callbacks на worker threads.

`Window` не является thread-safe и не выполняет внутреннюю synchronization для
доступа с нескольких потоков.

Если конкретный framework имеет более строгие ограничения, concrete
implementation обязана соблюдать это ограничение дополнительно.

Engine worker threads не должны напрямую вызывать Window API.

## GLFW implementation

GLFW является первой concrete implementation за внутренним `IWindowEngine`.

Публичные headers `Window` не включают GLFW headers и не раскрывают `GLFWwindow*`.
Callbacks GLFW преобразуются внутри модуля в `OnResize()`,
`OnFramebufferResize()` и `OnClose()`.

`WindowRuntime` фиксирует application thread для оконного runtime. Для
standalone использования `Window` объект `WindowRuntime` должен быть создан на
application/process main thread до создания GLFW-window и жить дольше окон.
`Application` делает это автоматически.

GLFW runtime инициализируется лениво после регистрации `WindowRuntime` и
завершается после уничтожения последнего active window. Несколько окон
разделяют один runtime ownership и один зарегистрированный application thread.

На зарегистрированном application thread должны выполняться `glfwInit`,
`glfwTerminate`, создание и уничтожение всех GLFW-window, а также event
processing. GLFW implementation проверяет этот thread перед window operations.

GLFW callbacks не вызывают пользовательские `Window::On*()` напрямую. Callback
только кладёт framework-independent событие во внутреннюю очередь, а
`ProcessEvents()` диспатчит накопленные события после возврата из
`glfwPollEvents()`.

Window не создаёт Vulkan surface и не создаёт OpenGL context. Для GLFW-window
используется `GLFW_CLIENT_API = GLFW_NO_API`; связь window с graphics backend
должна жить в отдельном integration layer.

GLFW main-thread requirement остаётся требованием concrete implementation и не
добавляет GLFW types/includes в framework-independent публичный API.
