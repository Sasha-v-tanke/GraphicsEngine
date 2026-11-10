# Семантическое завершение Update

`IFrameRuntime::ScheduleUpdate(frame, tasks, completion)` может вернуть до завершения систем. Default implementation вызывает legacy `Update` и передаёт успех/исключение в completion. ECS runtime вызывает `ScheduleWorldUpdate` с настоящим `shared_ptr<World>`. Для Scene используется aliasing shared_ptr, удерживающий SceneManager::Acquire/AcquireActive; слабый lifetime token не является владением.

Adapter выполняет PrepareSystems → GetHandle → BindCancellation → Start. Запись completion существует до первой публикации и допускает inline Finish. При поздней привязке после Stop cancellation вызывается сразу вне runtime locks. Finish разрешён после всех systems, producer tickets, структурного commit и уничтожения временных payloads; повторный сигнал игнорируется. Конкретный FrameKey захвачен владельцем completion: сигнал не может завершить другую генерацию слота.

Engine отдельно хранит orchestration TaskHandle и semantic completion. Main OnDraw ждёт semantic success; ранний legacy Draw регистрирует намерение, которое превращается в задачу лишь после успешного completion. Задача рисования также зависит от возврата оркестратора. Ошибка/отмена закрывает admission и не запускает Extract. Слот не освобождается в catch при живых CPU consumers.

Stop закрывает admission, запрашивает cancellation всех completion records вне metadata lock и ждёт их quiescence до WaitIdle. Это существенно для producer tickets, которые могут пережить idle TaskSystem. Пул жив до окончания принятых wrappers, callback tails и Drain. Повторный запуск получает новые FrameKey. Completion callbacks пробуждают main через платформенный wakeup вне runtime metadata locks; bounded maintenance ожидание сохраняется для GPU.

Runtime обязан вызвать Finish либо передать completion адаптеру, гарантированно завершающему его после quiescence. Нельзя захватывать TaskContext& или frame arena pointers в незарегистрированную долгоживущую работу. Синхронный RunSystems(TaskSystem&) остаётся host-only.

## Контрольные точки Update/Draw

`Engine::Update()` допускает следующий кадр через существующие World/slot gates. Принятый `Draw()` уже завершает application checkpoint текущего кадра и увеличивает `m_nextDrawFrameIndex`; `DrawRequested=true` может предшествовать `HasDrawTask=true`. В этом промежутке повторный `Update()` возвращает false через `TryBeginFrameLocked`, не бросает out-of-order и не запускает callback повторно. Draw/Extract по-прежнему требуют semantic Update success. После Extract открывается World gate, но физический слот ждёт всех CPU/GPU consumers. Два Update без принятого Draw, Draw до Update и повтор Draw остаются INVALID_STATE.

Воспроизведение на `a06282d` (исходный failing main) с временной трассой старого guard:

```txt
Update(0) -> Draw intent(0) -> Update retry
next=1 reservation=1 updateTask=1 drawRequested=1 drawTask=0 updateSucceeded=0 admissionOpen=0
Application update checkpoint is out of order: Invalid state
```

CPU regression удерживает semantic completion без блокировки worker. Матрица workers=1/2/8 × slots=1/2/3 выполняет по 12 кадров, проверяет отсутствие раннего Extract/Draw, прогресс независимой job, bounded GPU backpressure и retirement frame storage. Отдельно проверяются неверные checkpoints, failure/cancel, Stop и stale completion после restart. До исправления два новых теста падают; production guard отличается только проверкой `!DrawRequested`.

Команды проверки после сборки:

```sh
GraphicsEngineEngineTests --gtest_filter='EngineLegacyCheckpoint.*'
VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT \
  GraphicsEngineVulkanBackendTests --gtest_filter=VulkanFrameContext.RecyclesEngineSlotsAfterTimelineEvents
```

GPU smoke сохраняет Required validation, 12 кадров, WorkerCount=1 и slots=1/3. Retry использует существующий progress wait вместо spin. Fixture публикует счётчик Completed перед promise: это устраняет отдельную гонку наблюдения счётчика после Stop, не меняя критерий GPU completion или assertions.
