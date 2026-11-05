# Семантическое завершение Update

`IFrameRuntime::ScheduleUpdate(frame, tasks, completion)` может вернуть до завершения систем. Default implementation вызывает legacy `Update` и передаёт успех/исключение в completion. ECS runtime вызывает `ScheduleWorldUpdate` с настоящим `shared_ptr<World>`. Для Scene используется aliasing shared_ptr, удерживающий SceneManager::Acquire/AcquireActive; слабый lifetime token не является владением.

Adapter выполняет PrepareSystems → GetHandle → BindCancellation → Start. Запись completion существует до первой публикации и допускает inline Finish. При поздней привязке после Stop cancellation вызывается сразу вне runtime locks. Finish разрешён после всех systems, producer tickets, структурного commit и уничтожения временных payloads; повторный сигнал игнорируется. Конкретный FrameKey захвачен владельцем completion: сигнал не может завершить другую генерацию слота.

Engine отдельно хранит orchestration TaskHandle и semantic completion. Main OnDraw ждёт semantic success; ранний legacy Draw регистрирует намерение, которое превращается в задачу лишь после успешного completion. Задача рисования также зависит от возврата оркестратора. Ошибка/отмена закрывает admission и не запускает Extract. Слот не освобождается в catch при живых CPU consumers.

Legacy `Update() → Draw()` разделяет порядок вызовов и готовность задач: принятый Draw завершает пользовательский checkpoint, даже если его задача ещё не опубликована. Следующий Update возвращает `false`, пока semantic Update/Extract удерживают World gate или следующий mapped slot занят CPU/GPU consumers. Отсутствие draw task само по себе не является ошибкой порядка. Повторный Update без принятого Draw, Draw без Update и повторный Draw остаются `INVALID_STATE`; scheduler checkpoint переключается при публикации draw task, до повторного открытия admission.

Stop закрывает admission, запрашивает cancellation всех completion records вне metadata lock и ждёт их quiescence до WaitIdle. Это существенно для producer tickets, которые могут пережить idle TaskSystem. Пул жив до окончания принятых wrappers, callback tails и Drain. Повторный запуск получает новые FrameKey. Completion callbacks пробуждают main через платформенный wakeup вне runtime metadata locks; bounded maintenance ожидание сохраняется для GPU.

Runtime обязан вызвать Finish либо передать completion адаптеру, гарантированно завершающему его после quiescence. Нельзя захватывать TaskContext& или frame arena pointers в незарегистрированную долгоживущую работу. Синхронный RunSystems(TaskSystem&) остаётся host-only.
