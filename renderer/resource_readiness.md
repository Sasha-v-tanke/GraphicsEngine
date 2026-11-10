# Готовность ресурсов кадра

## Политика MVP

`RenderableComponent::ReadinessPolicy` явно выбирает `EResourceReadinessPolicy::SKIP_OBJECT`. Если необходимая сетка или материал не готовы, весь объект исключается из RenderWorld. Это выбранная политика для текущего frontend; автоматических placeholder, fallback assets или синхронной загрузки нет. Будущая fallback policy может расширить этот контракт, сохранив получение immutable leases и диагностику решений.

Для валидных handles состояния UNLOADED, QUEUED, LOADING, CPU_READY, UPLOADING, FAILED и UNLOADING не дают lease. CPU_READY отдельно не означает разрешение GPU consumer: async loader с uploader публикует READY только после exact upload completion. CPU-only загрузчики сохраняют существующий READY-контракт для backend preparation.

Пустой handle означает отсутствие обязательного ресурса и также даёт skip. Foreign, stale и type-mismatched handles остаются ошибками программирования: extraction fail-fast с object ID, ролью ресурса и slot/generation, как и до введения readiness policy. Неизвестное значение policy отклоняется. Невалидный handle не превращается в ссылку на новый ресурс, занявший тот же slot.

## Граница снимка

`ResourceManager::Observe<T>()` под одним metadata lock получает состояние, identity, failure и lease конкретной READY-версии. Нет гонки между независимыми GetState/GetFailure/TryAcquire. Копии строк принадлежат наблюдению; lease удерживает immutable CPU/GPU payload. Наблюдение неверного поколения не раскрывает identity или payload заменившего его ресурса. Observe не выполняет I/O, upload или ожидание completion; краткая синхронизация метаданных остаётся частью thread-safe Resources API.

При последовательной extraction каждое наблюдение сразу превращается в объект или диагностику skip. При параллельной extraction наблюдения и leases получаются в фазе сбора входов, до планирования chunks. Chunks читают только захваченные результаты и render-facing компоненты, затем join публикует снимок. READY/retry/unload после этой фазы не меняет уже принятых решений, включая случай, когда chunks ещё не начали исполняться. World остаётся read-only до join по существующему контракту.

Наблюдения разных ресурсов не являются глобальной транзакцией ResourceManager. Каждый ресурс наблюдается атомарно в момент сбора; переходы, совпавшие с самой фазой сбора, могут быть видны последующим наблюдениям. Опубликованный RenderWorld всегда immutable. Renderer::Prepare/SchedulePrepare не опрашивают ResourceManager и не заменяют версии или решения уже извлечённого кадра.

## Диагностика

`RenderWorld::GetResourceDiagnostics()` возвращает frame-owned записи только о недоступных обязательных ресурсах: object ID, роль MESH/MATERIAL, policy, запрошенный slot/generation и скопированный ResourceStatus. При известном ресурсе доступны logical identity, lifecycle state и исходный ErrorInfo с path/context. Для пустого handle identity/state отсутствуют, Reference равен EMPTY. Ошибка исходного загрузчика сохраняется без изменения и остаётся доступной в старом кадре после retry, unload и Forget.

При двух недоступных ресурсах публикуются обе причины, сначала mesh, затем material. Порядок объектов и объединения chunks совпадает с последовательным обходом World и не зависит от worker completion. Для готовых объектов диагностик нет. Данные принадлежат FrameStorage; нельзя сохранять span после освобождения соответствующего slot. Если диагностика нужна дольше, скопируйте значения.

## Lifetime и подготовка

Отброшенный объект не создаёт draw command или resource use record. Принятый объект хранит leases конкретных версий; resource use records переносят это владение на границу отправки. Ни unload, ни повторное использование resource slot не инвалидирует уже захваченный payload. CPU/GPU representations освобождаются после последнего lease/use record согласно существующему completion contract. Этот модуль не вводит новую native retirement queue и не меняет ownership backend.

FrameStorage владеет нетривиальными контейнерами диагностики и leases и уничтожает их до reset арены. RenderPlan не добавляет ссылки на эти строки: consumers читают диагностику через живой RenderWorld, а preparation использует только его удерживаемые версии.

## Проверки

Регрессионные тесты выполняют delayed I/O -> UPLOADING -> READY при трёх активных кадрах, failure/retry и сохранение identity/path, unload/Forget после capture, удержание GPU token отдельным use record, публикацию READY до начала gated chunks и сопоставление serial/parallel diagnostics. Запуски с 1/2/8 workers проверяют прогресс TaskSystem без ожиданий загрузки и независимость результатов от порядка завершения. Отдельные Resources tests проверяют атомарность статуса/lease во время concurrent reload и отсутствие stale/foreign identity leakage.
