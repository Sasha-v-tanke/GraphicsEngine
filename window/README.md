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
