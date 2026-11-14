# Инкрементальное извлечение RenderWorld

`NRenderer::NInternal::IncrementalExtraction` — внутренний поток снимков, используемый
`SceneRuntime`. Старые serial/parallel функции остаются эталонным плоским путём.
Один экземпляр принимает только одно незавершённое извлечение; разные потоки могут
независимо читать один quiescent World. TaskSystem, Resources и frame storage должны
пережить возвращённое completion. Уничтожение facade не отменяет отправленные задачи.

## Граница и версии

Engine завершает mutable producers и structural commit перед ScheduleExtract.
На этой границе extraction публикует pending dirty summary и читает immutable
ChangeSnapshot. Домены TransformComponent, CameraComponent и RenderableComponent
определяют обновляемые входы. Scoped ranges перечитывают только затронутые компоненты;
legacy mutable ссылки дают coarse invalidation. Пользователь не должен сохранять
writable ссылки для записи вне объявленной фазы: tracker не обнаруживает raw writes.

Первый кадр, смена World/поколений, изменение топологии, whole-storage invalidation
или потеря dirty history запускают полную повторную выборку. Идентификатор World,
а не адрес объекта, отделяет уничтоженный World от нового по тому же адресу.
Неизменившийся кадр не делает Query, resolve, copy output vectors или новые задачи
на объекты. Он удерживает прежнее immutable backing storage в своём FrameStorage.

Material/mesh наблюдаются один раз на уникальный typed slot/generation в каждом
извлечении, с проверкой owner identity. Это необходимо и без изменений ECS:
асинхронная загрузка, retry и unload не изменяют render component. Решение READY/skip
и ResourceLease фиксируются до отправки продолжения. Версии, прочитанные позднее,
не могут изменить уже захваченный кадр. Отсутствующий ресурс использует существующий
SKIP_OBJECT, ошибки сохраняют ResourceStatus. Stale/foreign references отклоняются.
Lease дублируется только внутренним builder без повторного acquisition текущей
версии; публичная move-only модель ResourceLease остаётся прежней.

## Иерархия и матрицы

При dirty inputs TransformResolver получает объединение всех камер и render
кандидатов, включая предков. Продолжение зависит от успешного resolver completion;
worker не вызывает Wait. World-компоненты копируются на границе и после неё не
читаются. Resolver удерживает topology lease до retirement обхода.

Итоговая матрица = resolved hierarchy world matrix × ComposeTransform прежнего
TransformComponent. Последний сохраняет своё поведение как плоское смещение
render объекта. Для старых сцен hierarchy matrix единичная. Не следует одновременно
записывать одно и то же смещение в оба представления. Точное affine значение
передаётся через RenderView/RenderObject и DrawCommandData до execution validation
и backend push constants. Shear не раскладывается в TRS. Perspective camera использует
обратную точную матрицу; clip-space camera по-прежнему имеет единичный view/projection.
Singular/nonfinite camera отклоняется. Culling/bounds сейчас отсутствуют: все кандидаты
остаются required, поэтому движение родителя не скрывает объект из-за старых bounds.

## Дельты и владение

RenderWorld по-прежнему предоставляет полный immutable снимок. GetDelta добавляет
WorldId, PreviousFrame, FullRebuild и deterministic ID списки changed/removed objects
и views. Потребитель применяет дельту только к предыдущему кадру этого потока.
Если PreviousFrame/WorldId не совпали либо FullRebuild=true, он строит состояние из
полного снимка. Списки — ID, значения находятся в текущем снимке. Изменение готовности
удаляет/добавляет объект тем же способом, что и структурное изменение.

Успешная публикация заменяет cache/cursor; exception и cancellation их не продвигают.
Пользовательские деструкторы ресурсов исполняются вне cache mutex. Reset разрешён
только idle и освобождает cache; сохранённые FrameStorage продолжают удерживать
старые payload/GPU representations независимо от Scene unload и текущего Resources.
Engine Stop сначала завершает CPU work, затем Drain освобождает cache. При ручной
отмене completion пользователь должен завершить TaskSystem work до изменения
топологии: отмена зависимой задачи сама по себе не присоединяет работающий resolver.

## Стоимость и проверка

Sparse изменения не создают task-per-entity. Продолжение обрабатывает batch, resolver
использует существующий adaptive traversal. При изменениях полный contiguous output
пока материализуется за O(render candidates), включая копии владения неизменившимися
ресурсами. Дельта содержит только фактические изменения матриц/версий/проекций.
Это сохраняет span API текущего renderer; persistent GpuScene/chunked snapshot —
последующее развитие. Даже static frame имеет O(candidates) сравнение выбранных
ресурсов и O(unique resources) обращений к ResourceManager; O(1) для сцены не обещается.

Регрессии Incremental проверяют WorkerCount=1/4 и MaxActiveFrames=1/2/3, old snapshots,
scoped single-object write, parent/shear, retry/hot reload, create/destroy/recycle,
missing history, смену World, capture-before-ready и failure rollback. Benchmark
BM_RenderWorldExtraction сравнивает неизменившийся кадр с parallel IMPROVE-0023:
1024/16384 объектов, 1/4 workers, режим 0 — baseline, 1 — incremental.
