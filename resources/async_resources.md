# Async Resources

## Владение и использование

Создайте `AsyncResourceLoader` на host/backend owner thread после ResourceManager и TaskSystem. Передайте ResourceIdentity, путь, memory-only decoder и необязательный uploader. Встроенные LoadImage/LoadMesh/LoadShaderArtifact используют тот же CPU-разбор, что синхронные загрузчики. Decoder получает bytes и stop_token, выполняется в TaskSystem; он обязан завершаться и проверять отмену в безопасных точках. Вызов готовых сторонних парсеров может завершиться только после возврата парсера. ResourceIo выполняет только чтение и handoff.

Параллельные запросы одной identity/type атомарно объединяются до запуска I/O, даже между несколькими загрузчиками с общим manager. Первый принятый запрос определяет путь/decoder/uploader. Повтор READY возвращает текущий handle; FAILED допускает новую operation generation. В identity должны входить import options (включая shader stage). Ошибки I/O, decode, submit и completion — FAILED с GetFailure; frame workers не должны ждать состояния.

Владелец backend регулярно вызывает PumpUploads. CPU-only работа публикуется из CPU_READY; uploader вызывается только здесь, получает immutable CPU payload и возвращает ResourceUpload: owning representation, неблокирующий IsComplete и точный Wait для shutdown. READY публикуется только после IsComplete()==true. Никакого device/queue idle или ожидания upload на compute/frame workers нет. CPU_READY наблюдаемо отдельно; Renderer skip/fallback не входит в этот слой.

MaxPendingReads ограничивает также число записей pipeline, включая decode и незавершённые uploads. Полная/остановленная служба переводит новый запрос в recoverable FAILED. Завершённые записи освобождаются PumpUploads, поэтому tick нужен и для CPU-only загрузок. Реализация не обещает zero-lock admission; короткие mutex защищают метаданные, файловые операции/декодирование/ожидания под ними не выполняются.

## Отмена и версии

Cancel(handle) запрашивает cooperative stop у принятых операций этой identity, отменяет ResourceIo и переводит только совпадающие активные поколения в UNLOADED. Можно сразу повторить Load. RequestUnload/CompleteUnload остаются поддержанными напрямую: все последующие стадии проверяют operation generation; эти вызовы не прерывают уже начатый системный вызов или сторонний decoder. Forget/reuse slot также запрещает stale-публикацию.

Отменённый upload остаётся удержанным до exact completion, даже при retry. Завершение старой операции не изменяет новую. Cancel после READY не выгружает готовую версию; используйте RequestUnload/CompleteUnload. CPU payload и representation объединяются в immutable owning version; lease.GetGpuRepresentation() возвращает backend token, а GetUseRecord сохраняет обе части для существующего механизма completion/lifetime.

Concrete native representation принадлежит backend. Его destruction должен быть безопасен на потоке последнего владельца lease либо backend обязан удержать allocation в своей owner-thread retirement queue. Device/allocator/state tracker должны пережить все native representations и потребителей. Этот контракт не заменяет GPU-use tracking последующих draw submissions.

## Адаптер Vulkan

В uploader создайте backend representation, затем вызовите существующий VulkanUploadManager::UploadBuffer/UploadImage/UploadBatch. Верните token с callbacks, замкнутыми на конкретный VulkanQueueCompletion: IsComplete вызывает VulkanSubmissionManager::IsCompleted(token), Wait — Wait(token, timeout). Backend удерживает allocation на owner thread и освобождает через свою retirement policy. UploadManager удерживает staging/destination до timeline completion; Resources удерживает опубликованную версию до освобождения leases. Vulkan heavy test `VulkanAsyncResources.DecodesAndUploadsMeshBeforeReady` проверяет полный OBJ -> ResourceIo -> TaskSystem -> batched staging -> timeline -> READY -> readback и logical unload под validation.

Uploader обязан возвращать валидный representation и оба callbacks либо бросать исключение, сохраняя безопасность уже отправленных команд на стороне backend. Poll может бросить: ресурс станет FAILED, но ticket сохраняется для точного drain при shutdown. Wait обязан завершить владение отправкой, в том числе при отмене; при device loss backend обязан обеспечить безопасный terminal cleanup перед исключением.

## Завершение

Остановите производителей запросов, затем вызовите Shutdown на constructor thread, до остановки TaskSystem и уничтожения backend. Shutdown запрещает admission, отменяет I/O/decode, присоединяет ResourceIo callbacks, ждёт только собственные задачи (включая retirement execution captures), затем drains уже отправленные uploads. Незавершённые CPU стадии становятся UNLOADED; опубликованные READY сохраняются. Shutdown может ждать внешнего системного вызова, cooperative decoder или GPU. PumpUploads/Shutdown не вызываются из decoder/uploader/completion callbacks и не перекрываются. Деструктор выполняет Shutdown; lifetime уничтожения с конкурентными публичными вызовами требует внешней координации.
