# Жизненный цикл сцены

`NScene::Scene` владеет одним миром ECS `World` и описательным именем. Пользователь может наследовать `Scene` для хранения состояния уровня сцены. `World` остаётся контейнером сущностей, компонентов и систем; `Scene` не создаёт второй ECS. Она не владеет `Graphics`, Vulkan или общим `ResourceManager`. Компоненты сохраняют логические дескрипторы ресурсов в соответствии с существующим контрактом `Resources`.

## Реестр и активация

[Модуль](module.cmake) зависит от `Ecs` и `Common`. Пользовательская навигация: [docs](../docs/README.md).

`SceneManager` владеет регистрациями и выбирает не более одной активной сцены. `CreateScene()` создаёт базовую сцену, `RegisterScene(unique_ptr<Scene>)` принимает подготовленную пользовательскую сцену. Ни одна операция не активирует её автоматически. `Activate(handle)` выбирает или переключает активную сцену, `Deactivate()` снимает выбор. Выгрузка активной сцены оставляет явно пустое состояние без неявного переключения на другую зарегистрированную сцену.

Операции реестра выполняются в создавшем менеджер потоке; обращения из других потоков отклоняются. Публикуйте активацию и выгрузку на контролируемой границе кадра приложения. Этот слой не добавляет ввод-вывод, автоматическое подключение к основному циклу, потоковую загрузку, сериализацию или асинхронную политику переключения и готовности ресурсов. Дескрипторы `SceneHandle` являются идентификаторами записей реестра с поколением, а не постоянными GUID или сериализованными идентификаторами сущностей.

## Идентичность и leases

Дескриптор содержит идентификатор менеджера, индекс слота и поколение. Выгрузка сразу делает регистрацию недействительной и запрещает последующие получения сцены по прежнему дескриптору. Повторное использование слота увеличивает поколение; при исчерпании поколений слот выводится из обращения. Устаревшие, чужие и незарегистрированные дескрипторы отклоняются. Повторная активация уже активной сцены не изменяет состояние.

`Acquire()` и `AcquireActive()` возвращают разделяемое владение `Scene`. Удерживайте его в асинхронных задачах над `World`, в том числе при извлечении данных для рендеринга: логическая выгрузка тогда не уничтожит исходный мир. Удержание владения не разрешает конкурентные записи в `World`; необходимо соблюдать декларации доступа ECS, фазы структурных изменений и контракт ожидания извлечения. Заимствованная таким образом сцена может пережить реестр или менеджер, но выгруженный дескриптор от этого не становится действительным.

## Снимки рендеринга

После завершения извлечения `RenderWorld` содержит копии данных для рендеринга и удерживаемые `ResourceLease` в слоте кадра. Он больше не читает `Scene` или `World`. Поэтому переключение или выгрузка может освободить старую `Scene`, пока её снимок остаётся действительным до завершения работ процессора и графического устройства для данного слота. `SceneManager` не выгружает общие ресурсы: их владелец управляет логической выгрузкой, а удержание версии в кадре или отправленной работе — её фактическим временем жизни.

## Остановка

При остановке необходимо прекратить публикацию новых работ со сценой и дождаться завершения обновления и извлечения `World` до освобождения используемых ими служб. Уничтожение `SceneManager` удаляет регистрации, но не отменяет задачи и не делает их удерживаемые сцены недействительными. Деструктор `Scene` выполняется в потоке, освободившем последнего разделяемого владельца; им может быть рабочий поток. Пользовательское состояние и деструкторы компонентов должны это допускать. Очистку с привязкой к потоку выполняйте на явной границе остановки владельца, удерживая `Scene` до окончания чтения. Во время уничтожения менеджера нельзя обращаться обратно к нему.

`CreateEntity()` и `EntityRef` дают пользовательский доступ к существующему World. См. [создание сущности](../docs/how_to/create_entity.md) и [модель доступа и lifetime](../docs/about/entity_ref.md).

## Opt-in metadata типов

`NCommon::TypeRegistry` из `<GraphicsEngine/lib/common/type_registry.h>` — самостоятельная часть
`Common`, не владеющая Scene/World. Обычным ECS-компонентам регистрация metadata не требуется.
Разные модули явно добавляют свои типы в один выбранный владельцем registry:

```cpp
struct Position {
    float X = 0;
};
NCommon::TypeRegistry types;
types.Register<Position>({0x1001}, "Game.Position", 1, NCommon::ETypeKind::COMPONENT);
types.Freeze();
auto metadata = types.Get<Position>();
auto value = types.Make<Position>(Position{.X = 3});
auto copy = value.Copy();
auto moved = value.MoveValue(); // value is empty; moved owns a newly move-constructed Position.
```

`TypeId` — назначенное автором ненулевое 64-битное значение. Его не вычисляют из `typeid`, имени,
пути файла или порядка регистрации; переименование типа не меняет ID. Автор обеспечивает уникальность
ID во всей persistent schema, не только внутри одной Scene. `GetCppType()` — только локальная связь с
C++ типом; она не предназначена для файлов или протоколов. Имена полностью задаются вызывающим модулем,
непустые и без встроенного NUL; версия схемы — положительный `uint32_t`.

`Register<T>(id, name, version, kind)` проверяет уникальность C++ типа, ID и имени; совпадение даёт
`ALREADY_EXISTS`, несовпадающая версия при таком конфликте — `INVALID_ARGUMENT`. Повторная регистрация
не заменяет старый descriptor. Ошибка/нехватка памяти не публикует частичную запись. Enum автоматически
получает `ENUM` и сведения о размере/знаковости underlying type; остальные типы — `VALUE` либо явно
`COMPONENT`. Несовместимый kind отклоняется. Размер, выравнивание и capabilities выводятся из C++ типа.

Владелец выполняет регистрацию и `Freeze()` в создавшем потоке. До закрытия любой lookup/creation
даёт `INVALID_STATE`; после закрытия регистрация навсегда запрещена. Повторный `Freeze()` идемпотентен.
Закрытие публикует immutable metadata с release/acquire ordering; `GetTypes()` перечисляет их в порядке
явных ID, независимо от порядка регистрации модулей. Параллельные `Find/Get/Create/Make` разрешены
после publication. Конструкторы и сами пользовательские значения требуют своей синхронизации.
Lookup использует линейный поиск; нет глобального registry и фоновых задач.

`Find(id/name)` и `Find<T>()` возвращают `nullptr` для неизвестного типа; `Get(id)` и `Get<T>()` дают
`NOT_FOUND`. Возвращаемый `shared_ptr<const TypeDescriptor>` удерживает неизменяемые metadata даже
после уничтожения registry. Контейнерный span `GetTypes()` заимствован; не сохраняйте его дольше registry.
Уничтожение registry не должно совпадать с обращениями к нему.

`TypeValue` — move-only владелец отдельного C++ объекта, а не указатель внутрь ECS. `Create(id)` выполняет
default construction; `Make<T>(args...)` поддерживает типы без default constructor. `Copy()` явно копирует
объект. `MoveValue()` создаёт новый объект из `T&&`, затем уничтожает исходный; обычное перемещение самого
`TypeValue` лишь передаёт владение и доступно даже для immovable `T`. Typed `new/delete` сохраняют alignment
и вызывают правильный destructor. Нетипизированные указатели и ручные placement operations наружу не выдаются.
Неподдерживаемая операция даёт `UNSUPPORTED`. При исключении копирования/перемещения источник остаётся
живым с exception guarantee самого `T`; успешное перемещение освобождает его ровно один раз.
`Reset()` и destructors не бросают: зарегистрированный `T` должен быть nothrow-destructible. Значения и metadata
могут пережить registry; модуль с кодом операций `T` должен оставаться загруженным. `TryGet<T>()` проверяет
точный C++ тип, `Get<T>()` даёт `INVALID_ARGUMENT` для пустого/другого типа; на пустом владельце `GetType`,
`Copy` и `MoveValue` дают `INVALID_STATE`. Операции над одним владельцем не должны перекрываться или повторно
входить в него из пользовательского copy/move constructor.

Одна регистрация не предоставляет сериализацию: `Capabilities.Serializable` всегда `false`. Codecs,
property access и enumerator values — следующие этапы; raw pointers и память C++ объекта не
считаются portable serialized data. Reflection не даёт права читать/изменять live ECS вне access scope.

Opt-in [генератор metadata](../tools/metadata_codegen/README.md) создаёт typed registration и canonical manifest
из явных sidecars. `GetFields()` описывает стабильные PropertyId/имя/C++ тип/owner без указателя внутрь World;
`GRAPHICS_ENGINE_METADATA_FIELD` проверяет member/owner при компиляции. Ручной API остаётся совместимым.

## Проверка контракта

[scene_test.cpp](test/scene_test.cpp) покрывает stale/foreign handles, явное пустое active state, owner thread, worker leases после unload/уничтожения менеджера, извлечение старой сцены во время переключения и удержание aliasing World lease до quiescence async systems. Публичный `Scene` предоставляет имя, `GetWorld` и EntityRef facade; сериализация остаётся последующей задачей.

## Spatial bounds и records (FEATURE-0115)

`spatial_bounds.h` не зависит от Renderer или Physics. `SpatialBoundsComponent`
(также value-owned результат внешнего bounds provider) задаёт локальный AABB/sphere,
необязательную STATIC/DYNAMIC classification и ненулевую source revision.
При смене bounds/classification producer увеличивает revision. Замена компонента
через deferred remove/commit/add/commit следует обычной ECS policy: remove/add
одного компонента в одном epoch запрещены. Обычная запись payload/revision
в owner phase разрешена, затем публикация после phase/commit barrier.

`CaptureSpatialRecord` и `CaptureSpatialRecords` вызываются owner thread после
retirement producers/commit при quiescent World. Они не запускают scheduler,
не ждут tasks, не выполняют commit и не добавляют блокировки на components.
Прямые записи компонентов/SetLocalTransform должны закончиться до capture;
сам snapshot не превращает произвольную live запись в deferred command.
Capture не должен пересекаться со Scene unload/World mutation. При allocation/access
failure исключение выходит до возврата результата, частичная публикация отсутствует.

Records содержат только копии: SceneHandle + world-qualified generational EntityReference,
валидность, global AABB и enclosing sphere, classification, source revision и точную
цепочку child→root identity/local revision существующей topology. Изменение предка
и reparenting меняют token; hash collisions и второй transform cache отсутствуют.
Цепочка разрешается serial reference path за O(depth); это foundation contract,
не incremental spatial index или новый transform resolver. No stale pointer/World
lease сохраняется после capture. Старые records переживают unload, но их identity
не подтверждает текущую live entity: повторная валидация через capture обязательна.

Single capture сообщает SCENE_UNAVAILABLE/FOREIGN_ENTITY/STALE_ENTITY/DISABLED/
MISSING_BOUNDS/INVALID_BOUNDS; только VALID содержит Bounds. Component snapshot
публикует enabled candidates в стабильном порядке entity index; invalid bounds
сохраняются как diagnostics. Provider overload копирует supplied source синхронно,
но всё равно проверяет scene/entity/enabled state. Disabled bounds component не
влияет на независимый внешний provider; disabled entity подавляет оба adapters.

TransformSpatialBounds принимает affine matrix и консервативно покрывает rotation,
nonuniform/negative scale и shear. Double scene coordinates не зависят от render origin:
origin offset сюда не передаётся и не изменяет global bounds. Input/output coordinates
и радиус ограничены `SpatialBounds::MAX_COORDINATE` (1e12 scene units); NaN/inf,
инвертированный AABB, отрицательный радиус, projective matrix и выход за бюджет
дают nullopt/INVALID_BOUNDS. Нулевой объём допустим. Sphere — conservative enclosure,
не tight minimal sphere. Native handles, GPU culling, collision и World Partition
в эту задачу не входят.

`test/spatial_bounds.cpp` проверяет геометрию, invalid inputs, commit visibility,
disabled entities/components, stale/foreign/recycled identities, scene unload,
provider/classification, hierarchy revisions и deterministic snapshots. Installed
package consumer использует только публичный header и вызов capture.

## Static BVH (FEATURE-0116)

`static_bvh.h` строит pointer-free CPU index из value snapshots FEATURE-0115.
`StaticBvh::Build(scene, version, records, config)` берёт только VALID records с
явной STATIC classification; dynamic, unclassified и diagnostic records учитываются
в SkippedRecords. Malformed included AABB/identity/source revision, mixed Worlds,
чужая Scene, duplicate identity и превышение MaxRecords отклоняются до publication.
Построение/Refit выполняются вне worker-critical Update после owner-thread capture:
caller может отправить value-owned inputs обычному TaskSystem, без worker Wait.
Отдельный scheduler, SceneManager, TransformResolver cache или GPU BVH не создаётся.

Builder использует median split по самой длинной оси AABB, tie X→Y→Z и полный
identity tie-breaker. Листья упорядочены по identity, shape/порядок nodes не зависит
от входного порядка. Complexity: build O(n log n), storage O(n), balanced depth
O(log n); на полностью перекрывающихся bounds queries остаются O(n). LeafCapacity
по умолчанию 8, диапазон 1–64; MaxRecords по умолчанию 1M, ограничение uint32/2
гарантирует безопасные node IDs и ограниченный stack traversal.

Снимок содержит immutable versioned nodes и компактные identity/AABB entries,
не удерживает World/Scene/components. Const queries параллельны без mutable counters.
Публикация принадлежит caller: например synchronized shared_ptr<const StaticBvh> на safe
boundary после завершения build. Caller назначает ненулевые монотонные версии
в пределах stream Scene. Старые snapshots остаются корректны для своих версий;
ни один rebuild/refit не меняет уже опубликованное дерево.

`Refit(newVersion, records)` требует строго новую версию и тот же набор static
identities; сохраняет topology/permutation и пересчитывает bounds bottom-up в новом
снимке. Меняются membership, generation или classification — нужен Build. Большое
перемещение может ухудшить pruning: Build восстанавливает median spatial grouping.
Refit не является dynamic AABB tree или change-driven синхронизацией FEATURE-0118.
Source/hierarchy revisions остаются в producer snapshot; index version связывает
геометрию с его publication, live revisions внутри BVH не читаются.

Ray/frustum/overlap возвращают AABB candidates в стабильном identity order, а не
geometry hits или ray-distance order. Контакт с boundary включается. Луч задаётся
`Origin + t * Direction`, MinDistance >= 0, MaxDistance >= MinDistance; направление
не обязано быть единичным, нулевой вектор запрещён, +infinity допустим только для max.
Frustum — шесть inward planes `dot(normal, point) + offset >= 0`, нормализация не
обязательна, нулевые normals запрещены. NaN/inf/inverted и out-of-budget inputs
отклоняются даже у пустого index. Query inputs используют тот же coordinate budget,
что SpatialBounds; origin-relative conversion может временно дать координаты до
двух budgets. Intermediate arithmetic округляется наружу: false negatives на границе
не допускаются, conservatively expanded candidates требуют обычного narrowphase.

Все query inputs допускают camera-relative coordinates с явным global origin.
Bounds в index не rebase/перезаписываются. Query результат содержит index version
и non-owning Scene-qualified identities. Перед доступом к live entity owner thread
вызывает `IsSpatialCandidateAlive`: unload, foreign World, stale generation и disabled
entity отклоняются. Это не проверка актуальности старой геометрии: caller обязан
использовать нужную snapshot version и собственный rebuild/refit policy. Смена Scene
не перепривязывает старую identity; old index не разыменовывает World после unload.

`GetStatistics().Serialize()` и per-query `Statistics.Serialize()` дают JSON schema 1:
version/input/indexed/skipped/nodes/leaves/depth/storage и visited/tested/matched.
StorageBytes — размер собственных vector capacities/Data, без allocator/control-block
надбавок, input SpatialRecords, внешних shared_ptr и query result buffers; это не RSS.
Latency не включается в deterministic counters и измеряется внешним scale harness.

`test/static_bvh.cpp` проверяет randomized brute-force parity, degenerate overlaps,
ray boundaries, relative origins, invalid inputs, deterministic nodes/candidates,
refit policy, stale/unloaded identities и rebuild/refit при concurrent readers.
`GraphicsEngineStaticBvhScaleTests` (label heavy) отдельно измеряет 1M bounds и 1000
localized queries, проверяет bounded memory/depth/pruning; запускается без GPU.
Installed package consumer собирает Build/Refit/query через public header.

Локальный измеренный baseline (Debug, Clang 23.1.1, этот Mac; не SLA):
1M records → 262143 nodes, 131072 leaves, depth 18, StorageBytes 124000240;
build 2285.59 ms; 1000 localized queries: 4.37 µs/query в среднем,
41.43 visited nodes/query. Query timing включает выделение result и asserts harness;
input records и allocator overhead не входят в StorageBytes. Команда повторения:
`cmake-build-ci-lite/GraphicsEngineStaticBvhScaleTests`. Измерения зависят от нагрузки
и конфигурации, а тест проверяет memory/depth/pruning, не hardware latency threshold.

## Dynamic AABB tree (FEATURE-0117)

`dynamic_aabb_tree.h` owns immutable versioned CPU indexes for VALID records
with explicit DYNAMIC classification. `Build(scene, version, records, config)`
creates the initial tree; `Refresh(newVersion, records)` consumes a complete,
authoritative capture after ECS commit (not a delta). CaptureSpatialRecords
provides source/transform revisions and global tight AABBs. The tree compares
geometry and identity; revision-only changes that leave geometry unchanged do
not require tree edits. Change-driven capture/facade belongs to FEATURE-0118.

Each leaf stores tight bounds and a conservative fat AABB expanded by FatMargin
(default 0.5 scene units, clipped to MAX_COORDINATE). Movement inside the fat box
only changes the tight box, outside movement/teleport removes and reinserts the
leaf. Absent or reclassified records are removed; generation replacement is
remove plus insert, not reuse of the old identity. Duplicate slots and malformed
included records, foreign Scene/mixed Worlds, invalid versions/configuration and
input budget overflow are rejected. The World identity, once known, is retained
even when the tree becomes empty. No Scene/World or component pointer is retained.

Insertion descends by minimum incremental surface-area cost; ancestors are refit
bottom-up, with height-based AVL rotations after structural changes. Freed node
slots are reused. Build is O(n log n); each membership edit or out-of-fat move
costs O(log n). Refresh copies the previous nodes/map and merges sorted input and
existing slots (O(n + k log n) for sorted capture with k structural changes);
unsorted input additionally costs O(n log n) to normalize. This deliberately
trades an O(n) snapshot copy for simple strong exception safety and immutable
concurrent readers. Retaining multiple versions retains their storage; no
sublinear snapshot-copy or lock-free publication guarantee is made.

QueryOverlap/QueryRay/QueryFrustum share the existing conservative StaticBvh
geometry and coordinate rules. Fat nodes prune traversal; tight leaf boxes are
tested to avoid fat-only false candidates. Results have stable identity order
and snapshot version. Caller publishes synchronized immutable snapshots at the
safe boundary and uses IsSpatialCandidateAlive before accessing live ECS
entities. Refresh never mutates a published version, even on failure. No worker
wait, Physics solver or automatic streaming policy is introduced.

Statistics report inserted/removed/reinserted/in-fat updates, rotations, depth
and allocated node slots per publication. `scene/test/dynamic_aabb_tree.cpp`
covers movement, teleport, recycled generations, admission failures, churn and
brute-force overlap/ray/frustum parity, concurrent readers and committed hierarchy
capture. `GraphicsEngineDynamicAabbTreeScaleTests` (heavy) compares incremental
refresh against complete dynamic/static rebuild on 50,000 records and 1% churn;
it validates candidate parity and structural-work counts and prints timings.
Small / Linux CI also runs this CPU-only test explicitly with verbose output,
without running the rest of the heavy suite. No measured speedup or hardware-specific threshold is asserted without running
the benchmark. Installed package consumer checks public Build/Refresh/query.
