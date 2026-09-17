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

Если конкретный framework имеет более строгие ограничения, например требует
создания и обработки окон только на process main thread, concrete implementation
обязана соблюдать это ограничение дополнительно.

Engine worker threads не должны напрямую вызывать Window API.
