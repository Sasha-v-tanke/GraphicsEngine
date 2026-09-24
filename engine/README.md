# Engine

## Engine ownership

`Engine` является owner/orchestrator CPU runtime.
Он владеет runtime subsystems и задаёт их lifetime:

- `TaskSystem`;
- `FrameScheduler`;
- будущие `World`;
- будущие `Resources`;
- будущие `Renderer`;
- будущие `Graphics`.

Подключение новых subsystems должно происходить как расширение owned runtime состава Engine.
Роль Engine при этом не меняется: он создаёт subsystems, запускает frame work, хранит runtime error channel и
останавливает runtime в безопасном порядке.

Публичный API Engine не содержит GLFW, Vulkan или других backend-specific типов.
Backend integration должна оставаться за private runtime/subsystem boundary.

## Lifecycle

Engine имеет состояния:

```text
CREATED -> RUNNING -> STOPPING -> STOPPED
```

`Start()` создаёт owned subsystems.
Если создание одного из subsystems падает, уже созданные части уничтожаются, Engine переходит в `STOPPED`, а исходная
ошибка пробрасывается вызывающему коду.

`Update()` и `Draw()` не выполняют весь frame pipeline синхронно.
Они только резервируют frame work и ставят его в `TaskSystem`.
Backpressure приходит от `FrameScheduler`: если следующий mapped frame slot не находится в `FREE`, `Update()`
возвращает `false`.
Это блокирует только application thread на checkpoint boundary. Worker threads не ждут освобождения frame slot:
они выполняют опубликованные update/draw tasks через dependency graph `TaskSystem`.
Runtime bookkeeping хранится в fixed per-slot records, индексированных тем же slot/generation identity, что и
`FrameHandle`.
Per-slot record удерживается до terminal completion draw task. Завершённые records reaping-ятся перед следующим
`Update()`, поэтому новый frame может быть принят только после безопасного recycle соответствующего slot.
Engine не использует отдельную heap FIFO очередь frame tokens.

## Runtime errors

Ошибки, возникшие внутри runtime work, сохраняются в Engine error channel.
Последняя ошибка доступна через `GetLastError()`.
`ClearLastError()` очищает канал.
Если runtime stage падает, Engine записывает ошибку, переводит runtime в `STOPPING` и не принимает новый frame work.
Исключение пробрасывается из worker callback дальше, поэтому `TaskSystem` помечает task как `FAILED`, а зависимые tasks
не получают успешного dependency completion.

## Shutdown order

`Stop()` переводит Engine в `STOPPING`, запрещая новый публичный frame work, затем ждёт завершения running/pending work
в `TaskSystem`.
После этого Engine reaping-ит completed frame records, abort-ит unresolved frame checkpoints, уничтожает frame/runtime
subsystems и переходит в `STOPPED`.
Такой порядок нужен, чтобы frame-local данные и будущие renderer/resource owners не переживали свои runtime owners.

## FrameScheduler

`FrameScheduler` управляет bounded lifetime одновременно активных кадров Engine.

Количество одновременно существующих frame slots задаётся через:

```cpp
NEngine::EngineConfig{
    .MaxActiveFrames = 2,
};
```

MaxActiveFrames должен быть больше нуля. Scheduler заранее создаёт фиксированный ring из MaxActiveFrames slots. Размер
ring не изменяется во время lifetime scheduler.

## Fixed ring

Физический slot кадра определяется только как:

```cpp
slotIndex = frameIndex % MaxActiveFrames
```

Scheduler не ищет другой свободный slot.
Если следующий mapped slot занят, новый frame не создаётся даже при наличии другого FREE slot.
Это сохраняет стабильное отображение logical frame index в bounded frame storage и является точкой backpressure для
последующей runtime-интеграции.
Правило одинаково для `MaxActiveFrames = 1` и для больших значений: различается только размер ring, а не семантика
acquire/recycle.

## State machine

FrameScheduler принимает внешние checkpoints как generation-bound signals внутри текущего FrameExecutionSlot:

```text
Update N -> Draw N -> Update N+1 -> Draw N+1
```

`TryAcquireFrame()` фиксирует Update signal для нового generation.
`SignalDraw()` фиксирует Draw signal того же generation.
Повтор checkpoint или skip порядка является ошибкой состояния.
Scheduler не хранит heap token queue: signals живут в физическом slot и инвалидируются reuse через slot generation.

Разрешённый lifecycle:

```text
           FREE
            |
            v
         ACQUIRED
            |
            v
       WAITING_UPDATE
            |
            v
         UPDATING
            |
            v
        WAITING_DRAW
            |
            v
         FINALIZE
            |
            v
          COMPLETE
            |
            v
           FREE
```

Другие переходы являются ошибкой состояния.
ACQUIRED отделяет резервирование frame slot от момента, когда frame полностью подготовлен и может ожидать Update
checkpoint.
FINALIZE нельзя начать до Draw signal соответствующего generation.
COMPLETE не означает, что slot уже доступен для следующего кадра.
Только RecycleFrame () выполняет:

```text
COMPLETE -> FREE
```

и делает физический slot доступным следующему generation.
Recycle выполняется только после полного completion всех work/users текущего frame. Для frame-local arena это safe
boundary: `FrameArena::Reset()` привязан к recycle и не выполняется при одном только draw checkpoint publication.

`AbortFrame()` является аварийным runtime contract для частично пройденного frame.
Он освобождает текущий generation из любого non-FREE состояния без прохождения обычных lifecycle transitions.
Engine использует его только после runtime failure или failed publication, когда обычная стадия больше не может быть
корректно завершена.

## Frame identity

FrameHandle идентифицирует конкретный lifetime frame и содержит:

- identity owning FrameScheduler;
- ApplicationFrameIndex;
- SimulationIndex;
- FrameSlotIndex;
- slot generation.

Handle может использоваться только с тем FrameScheduler, который его создал.
Handle другого scheduler отклоняется.
При каждом reuse физического slot его generation увеличивается.
После recycle предыдущий handle становится stale и больше не может использоваться для чтения состояния, выполнения
transition или получения frame-local storage.
Generation предотвращает ABA при повторном использовании того же физического slot.

Старые `GetFrameIndex()` и `GetSlotIndex()` остаются совместимыми aliases для ApplicationFrameIndex и FrameSlotIndex.

## Timing

FrameScheduler использует `std::chrono::steady_clock`.
DeltaTime считается при ordered Update signal в `TryAcquireFrame()`:

```text
DeltaTime(N) = start(Simulation N) - start(Simulation N-1)
```

Для первого Update delta равен zero.
Delta привязан к SimulationIndex/generation frame и доступен через `GetDeltaTime(frame)`.
Порядок worker-вызовов `BeginUpdate()` не влияет на DeltaTime.

## Frame-local storage

Каждый FrameExecutionSlot владеет собственным FrameArena.
Arena существует столько же, сколько физический slot, и может использовать один и тот же std::pmr::memory_resource в
нескольких generation.
Данные, выделенные из arena, принадлежат только текущему generation frame.
Полученные из arena:

- pointers;
- references;
- allocators;
- containers;
- objects

могут использоваться только до RecycleFrame () соответствующего frame.
После RecycleFrame () любые такие references и allocations считаются недействительными, даже если объект
memory_resource физически остаётся тем же.
Перед RecycleFrame () должны быть завершены все users frame-local data.
Lifetime нетривиальных C++ объектов должен быть завершён их владельцем до recycle. FrameArena::Reset () освобождает
storage, но не заменяет вызов destructors объектов.
После завершения всех owners/users scheduler выполняет reset arena и только затем переводит slot в FREE.

## Thread safety

Публичные операции FrameScheduler сериализуют изменение и проверку состояния через scheduler mutex.
Это обеспечивает согласованность:

- slot state;
- frame identity;
- generation;
- recycle.

Полученный memory_resource используется независимо от scheduler mutex и является thread-safe resource.
Корректность lifetime frame-local allocations обеспечивается правилом: никакой пользователь текущего generation не
должен переживать RecycleFrame ().

## Responsibility

FrameScheduler является private runtime component Engine.
