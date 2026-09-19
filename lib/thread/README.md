# Thread Module

`Thread` содержит backend-independent CPU scheduling utilities. Текущая публичная часть модуля - `TaskSystem`.

## TaskSystem

`TaskSystem` - thread pool для выполнения `TaskFunction` с dependency graph. Он не зависит от renderer, resources,
window system или graphics backend.

### Task Lifecycle

```text
CREATED -> WAITING -> READY -> RUNNING -> COMPLETED
                         |          |
                         |          +-> FAILED
                         +------------> CANCELLED
```

`CREATED` является внутренним pre-publication state. После возврата из `Submit()` пользователь может наблюдать только
`WAITING`, `READY`, `RUNNING` и terminal states.

- `WAITING`: задача опубликована, но ожидает успешного завершения prerequisites.
- `READY`: все prerequisites успешно завершены, задача находится в очереди worker pool.
- `RUNNING`: задача выполняется на worker thread.
- `COMPLETED`: callback завершился без исключения.
- `FAILED`: callback выбросил исключение, которое преобразовано в `ErrorInfo`.
- `CANCELLED`: задача отменена явно или из-за failed/cancelled prerequisite.

### Handles And Lifetime

`TaskHandle` - стабильная ссылка на опубликованную задачу. Handle привязан к конкретному instance identity
`TaskSystem` и opaque task state; локальный числовой id используется только для диагностики. Handle из другого
`TaskSystem`, включая уничтоженный scheduler и новый scheduler, созданный по тому же адресу памяти, никогда не alias-ит
локальную задачу и отклоняется как invalid argument.

Terminal state остаётся доступным через `TaskHandle`, пока пользователь хранит handle и соответствующий `TaskSystem`
жив. Scheduler при переходе в terminal state удаляет задачу из active lookup и освобождает execution payload: callback,
dependency edges и captured resources, которые удерживались только callback-ом. Поэтому память scheduler не растёт
пропорционально total submissions за всё время жизни `TaskSystem`.

После уничтожения `TaskSystem` оставшийся `TaskHandle` является stale value object. Использовать его с новым или другим
`TaskSystem` нельзя; такой handle должен быть отвергнут.

`TaskContext` живёт только во время callback. Его нельзя сохранять или использовать после возврата из `TaskFunction`.
`TaskContext::GetTask()` возвращает handle текущей задачи, `TaskContext::GetWorkerIndex()` - stable index worker thread,
на котором выполняется callback.

`TaskSystem` фиксирует `WorkerCount` при construction. Значение `0` нормализуется в `1`, после запуска pool размер не
меняется и доступен через `GetWorkerCount()`. `WorkerIndex` всегда находится в диапазоне `[0, GetWorkerCount())` и
стабилен для конкретного worker thread.

### Dependencies And Ordering

Dependency set копируется при `Submit()`. Последующие изменения контейнера, из которого был создан `std::span`, не
меняют dependency graph.

Задача становится `READY`, когда все prerequisites завершились `COMPLETED`. Если любой prerequisite завершился
`FAILED` или `CANCELLED`, dependent task переходит в `CANCELLED`. Несколько dependents и несколько prerequisites
поддерживаются.

Для независимых `READY` задач порядок выполнения не гарантируется. Зависимости между worker-задачами должны выражаться
через graph, а не через blocking wait внутри callback.

`READY` задачи находятся в общей очереди scheduler. Workers не привязаны к frame, subsystem или rendering stage:
любой idle worker может взять любую `READY` задачу. Application thread не выполняет worker callbacks; он только
публикует задачи, читает состояние и блокируется в `Wait()`/`WaitIdle()`.

### Error Boundary

Исключения не выходят за границу worker thread. Callback exceptions преобразуются в `ErrorInfo`:

- `NCommon::Exception`: сохраняются project `std::error_code` и diagnostic message.
- `std::system_error`: сохраняется исходный `std::error_code` и message.
- `std::exception`: `EError::UNKNOWN` и `what()`.
- unknown exception: `EError::UNKNOWN` и generic diagnostic message.

`GetError()` возвращает `std::optional<ErrorInfo>`. Для `COMPLETED` и `CANCELLED` задач ошибки нет.

### Thread Safety

`Submit()`, `GetStatus()`, `GetError()`, `Cancel()`, `Wait()` и `WaitIdle()` можно вызывать с внешних потоков
одновременно. `Wait()` и `WaitIdle()` запрещены из worker thread того же `TaskSystem`; worker callbacks должны создавать
новые задачи через `TaskContext::Spawn()` и выражать ordering через dependencies.

`Wait(task)` блокирует caller до terminal state задачи. Он не бросает из-за `FAILED` или `CANCELLED`: итог нужно читать
через `GetStatus()` и `GetError()`.

`WaitIdle()` блокирует caller до момента, когда нет running/active scheduler tasks. Внутренние tombstones в очередях,
оставшиеся после cancellation, не считаются outstanding work. Terminal task state может оставаться живым во внешних
handles.

Idle workers блокируются на scheduler wakeup primitive и просыпаются при публикации новой `READY` задачи или shutdown.
Ожидание idle state не требует busy spin.

### Ownership And Shutdown

`TaskSystem` владеет worker threads. Конструктор запускает pool и rollback-ит частично созданные workers, если запуск
одного из threads бросает исключение.

Деструктор переводит pool в stopping state, будит workers и join-ит все созданные threads. Уже взятые worker-ами задачи
могут завершиться; новые задачи после начала shutdown отклоняются.
