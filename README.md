# Graphics Engine

Движок для рендеринга графики на C++. Планируется поддержка Vulkan, OpenGL; GLFW, Qt etc.

## Setup

Установит pre-commit хук для форматирования коммита:

```bash
git config core.hooksPath .githooks
```

## Архитектура

GraphicsEngine разделён на независимые модули с явно определёнными зонами ответственности и зависимостями.

```text id="fai033"
Application
    │
    ├── Window
    │     └── GLFW / Qt / SDL
    │
    └── Engine
          ├── ECS
          ├── Resources
          ├── TaskSystem
          └── Renderer
                 │
                 ▼
              Graphics
                 │
          ┌──────┴──────┐
          ▼             ▼
       Vulkan         OpenGL
```

### Модули

- **Application** — жизненный цикл приложения и основной цикл. Пользователь может наследоваться от базового
  `Application` и переопределять необходимое поведение.
- **Window** — абстракция окна без зависимости от конкретной оконной библиотеки. GLFW, Qt, SDL и другие реализации
  скрыты за внутренними интерфейсами и фабриками.
- **Engine** — основной runtime и координатор подсистем. Связывает мир, ресурсы, renderer и систему задач.
- **ECS** — entities, components, systems и состояние мира. Не зависит от graphics backend.
- **Resources** — backend-independent ресурсы и управление их жизненным циклом: textures, meshes, materials, shaders и
  т.д.
- **TaskSystem** — общий thread pool и выполнение задач с учётом их зависимостей.
- **Render** — высокоуровневая подготовка отрисовки: extraction, culling, LOD, sorting, batching и построение render
  lists.
- **Graphics** — низкоуровневая backend-independent graphics abstraction, используемая renderer.
- **Vulkan / OpenGL** — реализации Graphics. Все API-specific объекты, synchronization, command recording и
  GPU-представления ресурсов остаются внутри соответствующего backend.

### Window abstraction

Пользователь выбирает тип оконной системы, но не работает напрямую с `GLFWwindow`, `QWindow`, `SDL_Window` и другими
native-типами.

```text id="o6fgux"
Window
    │
    ▼
IWindowImpl / IWindowSystem
    │
    ├── GLFW
    ├── Qt
    └── SDL
```

Window system и graphics backend являются независимыми подсистемами.

Места, где необходима информация сразу о двух сторонах, например создание Vulkan surface для GLFW, выносятся в отдельный
integration layer.

### Границы абстракций

- ECS не знает о Render, Vulkan или OpenGL.
- Resources не предоставляет наружу `VkImage`, OpenGL handles и другие backend-specific объекты.
- Render работает через backend-independent Graphics API.
- Graphics не зависит от конкретной оконной библиотеки.
- Vulkan/OpenGL implementation details не выходят за границы соответствующего backend.
- GLFW/Qt/SDL types не выходят в публичный Engine/Graphics API.
- Backend-specific поведение выбирается через capabilities backend, а не через проверки `if Vulkan` / `if OpenGL` в
  высокоуровневых модулях.
- Высокоуровневые системы зависят от абстракций, а конкретные реализации остаются изолированными внутри своих модулей.

## Frame Pipeline and Multithreading

### Общая модель

Engine использует **task-based multithreading**.

Стадии кадра не привязаны к конкретным потокам. Вместо `UpdateThread`, `RenderThread`, `ThreadPerFrame` работа
представляется как граф задач с зависимостями.

```text
Frame N

Update World N
      │
      ▼
Extract RenderWorld N
      │
      ├──────────────────────────────► Update World N+1
      │
      ▼
Render Preparation N
      │
      ▼
Backend Preparation N
      │
      ▼
Submit N
      │
      ▼
GPU Execute N
      │
      ▼
Frame Complete N
      │
      ▼
Recycle FrameContext N
```

`World` изменяется последовательно между simulation frames:

```text
Update N
   │
   ▼
Extract N
   │
   ▼
Update N+1
```

Но сам `Update N` может состоять из множества параллельных задач.

После `Extract N` создаётся независимый `RenderWorld N`, поэтому одновременно могут выполняться:

```text
World:       Update N+1
Render:      Prepare N
GPU:         Execute N-1
```

Это основная pipeline-модель для всех graphics backends.

---

### Разделение ответственности

#### World / ECS

Хранит текущее mutable-состояние мира.

```text
World
├── Entities
├── Components
└── Systems
```

Update состоит из задач:

```text
              Update
          /      |      \
         ▼       ▼       ▼
    Animation  Physics  Gameplay
         \       |       /
          ▼      ▼      ▼
            Transforms
                │
                ▼
             Extract
```

Независимые systems выполняются параллельно.

Зависимые systems запускаются только после выполнения необходимых предыдущих задач.

Два `Update` одного World одновременно не выполняются.

---

### RenderWorld

После World Update выполняется extraction:

```text
World N
   │
   ▼
Extract
   │
   ▼
RenderWorld N
```

`RenderWorld` содержит только данные, необходимые renderer'у.

После завершения extraction renderer больше не обращается к mutable `World N`.

Это позволяет одновременно выполнять:

```text
Update World N+1
        +
Render RenderWorld N
        +
GPU Execute N-1
```

---

### Render Preparation

Backend-independent подготовка кадра выполняется через общий `TaskSystem`.

`TaskSystem` — базовый backend-independent runtime для CPU-задач. Он не содержит renderer/resource/backend-specific API
и используется как общий механизм планирования работы внутри engine.

Жизненный цикл задачи:

```text
CREATED -> WAITING -> READY -> RUNNING -> COMPLETED
                         │          │
                         │          └── FAILED
                         └───────────── CANCELLED
```

Полный contract `TaskHandle`, task lifetime, dependency ordering, worker exception boundary и blocking API описан в
[`lib/thread/README.md`](lib/thread/README.md).

Пример графа:

```text
                    RenderWorld
                         │
          ┌──────────────┼──────────────┐
          ▼              ▼              ▼
       Culling          LOD        Materials
          │              │              │
          └──────────────┼──────────────┘
                         ▼
                   BuildRenderItems
                         │
                         ▼
                       Sort
                         │
                         ▼
                       Batch
                         │
                         ▼
                  DrawCommandData
```

Крупные операции дополнительно разбиваются на chunks:

```text
Culling
├── Chunk 0
├── Chunk 1
├── Chunk 2
└── Chunk 3
```

Chunks являются независимыми tasks и могут выполняться любыми workers.

---

### Backend Preparation

До этой точки pipeline одинаков для всех backends.

Дальнейшая многопоточность определяется возможностями конкретного backend.

#### Vulkan

Vulkan поддерживает параллельную запись command buffers:

```text
DrawCommandData
       │
 ┌─────┼─────┬─────┐
 ▼     ▼     ▼     ▼
Task  Task  Task  Task
 │     │     │     │
Sec0  Sec1  Sec2  Sec3
 └─────┼─────┴─────┘
       ▼
    Primary
       │
       ▼
     Submit
       │
       ▼
      GPU
```

Каждый worker использует собственный context текущего frame:

```text
FrameContext 0
├── WorkerContext 0
├── WorkerContext 1
├── WorkerContext 2
└── WorkerContext 3

FrameContext 1
├── WorkerContext 0
├── WorkerContext 1
├── WorkerContext 2
└── WorkerContext 3
```

`WorkerContext` может содержать:

```text
VkCommandPool
DescriptorPool
Transient allocators
Temporary backend resources
```

Таким образом `FramesInFlight = 2` и `WorkerCount = 4` означают 8 `WorkerContext`, но только 4 worker threads.

Primary command buffer выполняет secondary buffers. После этого производится submit.

`FrameContext` нельзя reset/reuse до завершения соответствующей GPU работы.

---

#### OpenGL

Вся backend-independent подготовка остаётся многопоточной:

```text
Culling
LOD
Animation
Sorting
Batching
Render item generation
...
```

Но окончательное выполнение OpenGL API сериализуется:

```text
DrawCommandData
       │
       ▼
 OpenGL execution
       │
       ├── glBind...
       ├── glDraw...
       ├── glDraw...
       └── ...
       │
       ▼
      GPU
```

Таким образом OpenGL backend не делает весь Engine однопоточным.

Ограничивается только backend-specific command execution.

---

### TaskSystem

Engine содержит общий thread pool:

```text
                    TaskSystem
                        │
        ┌───────────────┼───────────────┐
        ▼               ▼               ▼
     Worker 0        Worker 1        Worker 2 ...
```

Количество workers задаётся независимо от количества frames:

```cpp
TaskConfig{
    .WorkerCount = 4,
};

RenderConfig{
    .FramesInFlight = 2,
};
```

`ThreadsPerFrame` отсутствует.

Workers не принадлежат кадрам, системам или стадиям rendering.

Один worker последовательно может выполнить:

```text
Physics N
    ↓
CullingChunk N
    ↓
RecordSecondary N
    ↓
Animation N+1
```

---

#### Task dependencies

Каждая задача знает количество незавершённых dependencies:

```text
Task A ─────┐
            ├──► Task D
Task B ─────┤
            │
Task C ─────┘
```

Логически:

```cpp
Task D:
    RemainingDependencies = 3;
```

После завершения каждой зависимости:

```text
A complete -> 2
B complete -> 1
C complete -> 0
```

При достижении `0` задача становится `READY` и помещается в очередь TaskSystem.

`RemainingDependencies` обновляется atomic decrement-ом при завершении prerequisite. Та task, которая последней довела
счётчик dependent до `0`, активирует dependent и публикует её в READY queue.

Модель поддерживает:

- fan-in: несколько prerequisites для одной task;
- fan-out: одна prerequisite разблокирует несколько dependents;
- dynamic publication: running task может публиковать новые tasks через `TaskContext::Spawn()`;
- immutable dependencies: после publication dependency set задачи не меняется, новые constraints выражаются новой task;
- independent READY tasks: порядок выполнения не задан и не должен использоваться как контракт.

```text
CREATED
   │
   ▼
WAITING
   │
   │ dependencies == 0
   ▼
 READY
   │
   ▼
RUNNING
   │
   ▼
COMPLETED
```

Worker никогда не должен занимать поток ожиданием dependency.

Нельзя:

```text
Worker 0
    Task B
        wait(Task A)   <- worker заблокирован
```

Вместо этого:

```text
Task B waits logically

Worker 0
    выполняет другую READY task

Task A completes
    ↓
Task B becomes READY
    ↓
любой worker выполняет Task B
```

---

#### Task scheduling

Концептуально scheduler работает так:

```text
                 Waiting Tasks
                       │
              dependencies ready
                       │
                       ▼
                  Ready Queue
               /      |       \
              ▼       ▼        ▼
           Worker0 Worker1  Worker2
              │       │        │
              ▼       ▼        ▼
           Complete Complete Complete
              │       │        │
              └───────┼────────┘
                      ▼
             unlock dependents
```

В дальнейшем scheduler может использовать per-worker queues и work stealing, но это implementation detail `TaskSystem`.

Основной контракт остаётся тем же:

> задача исполняется любым доступным worker только после выполнения всех её dependencies.

---

### Frame synchronization

`FramesInFlight` ограничивает количество frame contexts, которые backend может одновременно использовать.

Например:

```text
FramesInFlight = 2

FrameContext[0] -> GPU still executing
FrameContext[1] -> CPU preparing/submitting

next frame wants FrameContext[0]
              │
              ▼
wait until GPU completed FrameContext[0]
              │
              ▼
reset/recycle
```

При этом simulation, render preparation и backend frame slots не должны искусственно иметь фиксированное смещение на
один кадр.

Scheduler запускает работу настолько рано, насколько позволяют зависимости и доступные frame resources.

---

### Итоговый pipeline

```text
               TaskSystem
         Worker 0 ... Worker N
                  │
                  ▼
World N ──► Update Task Graph
                  │
                  ▼
              Extract N
             /         \
            /           \
           ▼             ▼
   RenderWorld N      Update World N+1
           │
           ▼
    Render Task Graph
     /    /   \    \
   Cull LOD Sort  Batch
     \    \   /    /
           ▼
    DrawCommandData
           │
           ▼
     Backend Policy
       /        \
      ▼          ▼
   Vulkan      OpenGL
      │           │
 parallel       serial
 recording     API calls
      │           │
      └─────┬─────┘
            ▼
          Submit
            │
            ▼
           GPU
            │
            ▼
     Frame Complete
            │
            ▼
    Recycle FrameContext
```

Главное архитектурное правило:

**Engine описывает зависимости между работами, а не назначает работы конкретным потокам.**

Backend сообщает ограничения своей модели исполнения. `TaskSystem` выполняет максимально возможную параллельную
CPU-работу, а `FramesInFlight` управляет независимым pipeline CPU ↔ GPU.

## Подключение GraphicsEngine

GraphicsEngine устанавливается как CMake package и подключается через `find_package`.

Минимальное подключение:

```cmake
find_package(
    GraphicsEngine REQUIRED CONFIG
)

target_link_libraries(
    MyApplication
    PRIVATE
    GraphicsEngine::GraphicsEngine
)
```

`GraphicsEngine::GraphicsEngine` является единственным публичным CMake target библиотеки.

Внутреннее разделение GraphicsEngine на модули является implementation detail движка и не является частью публичного
CMake API.

### Components

Graphics и window backends представлены как capabilities установленной сборки GraphicsEngine.

Если приложению требуется конкретный backend, его можно указать через `COMPONENTS`:

```cmake
find_package(
    GraphicsEngine REQUIRED CONFIG
    COMPONENTS Vulkan GLFW
)
```

В этом случае конфигурация проекта успешно завершится только в том случае, если установленная сборка GraphicsEngine
содержит поддержку всех запрошенных components.

Components не являются отдельными библиотеками и не меняют способ линковки приложения. Независимо от выбранных
components приложение всегда использует:

```text
GraphicsEngine::GraphicsEngine
```

Примеры возможных components: `Vulkan`, `OpenGL`, `GLFW`, `Qt`.

Набор доступных components определяется конфигурацией, с которой был собран GraphicsEngine. Если приложение не требует
конкретной реализации graphics или window backend, `COMPONENTS` указывать не требуется.

До определения отдельной versioning и compatibility policy GraphicsEngine не гарантирует ABI compatibility между
версиями.
