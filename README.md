# Graphics Engine

Движок для рендеринга графики на C++. Планируется поддержка Vulkan, OpenGL; GLFW, Qt etc.

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