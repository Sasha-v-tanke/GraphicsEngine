# ECS concurrency acceptance

Проверки IMPROVE-0045 объединяют scoped scheduling, hierarchy, adaptive traversal,
ChangeTracker, TransformResolver, incremental extraction и Scene lifetime. Это CPU-набор:
Vulkan, фактический GPU completion, streaming I/O и общий R8 hardening проверяются отдельно.
Публичный ECS API и планировщик задач этот набор не меняет.

## Эталон и воспроизводимость

`forest.h` хранит собственные parent/children/local-matrix данные. Эталон не вызывает
`TopologySnapshot::GetWorldTransform`, `AdaptiveBatchPlanner::RunSerial` или resolver.
Матрицы перемножаются отдельным скалярным циклом; top-down/bottom-up значения вычисляются
итеративно по собственному списку связей. Порядок детей совпадает с порядком постановки
reparent-команд, включая некоммутативную bottom-up редукцию. Фиксированные seeds служат
для точного повторения topology и конфликтующих записей, а не для случайности безопасности.

Одинаковые сценарии повторяются при 1/2/4/8/16 workers. Порядок регистрации scoped-систем
перемешан; ожидаемый результат вычисляется по стабильным ключам систем. После каждой фазы
проверяются значения компонентов, отсутствие пропущенных dirty ranges и независимые
мировые матрицы. Параллельный callback не получает права на произвольный World: используются
существующие scoped views и quiescent structural boundaries.

| Проверка | Покрытие |
| --- | --- |
| `IndependentReferenceAcrossShapesWorkersAndTraversalModes` | balanced/star/bamboo/random/skewed, 1024 узла, все workers, TOP_DOWN/BOTTOM_UP/INDEPENDENT, tiny/heavy callback, exact once, bounded peak packets |
| `RandomScopedReplayVersionsReparentAndLazyMatricesMatchIndependentModel` | seeded conflicting writes, перемешанная регистрация, dirty coverage, shear/parent edits, reparent, cache hits, неизменяемость старого resolve, stale/recycled identities |
| `ConflictDiagnosticsCancellationAndOwnerIsolationMatchReplay` | реальные причины конфликтов AccessCoordinator, pending cancellation, разные owner domains, drain перед проверкой результата |
| `ShutdownMidTraversalDrainsCallbacksAndReleasesTopologyBeforeRestart` | явно отменённый работающий обход, уничтожение пула на host, retirement captures/topology, повторный запуск; 1/4/16 workers |
| `FailedAndCancelledScopedRunsDrainThenResolveAndPublishNoFalseSuccess` | ошибка после записи и отмена SystemRun, наблюдаемый outcome, conservative dirty state, quiescence и последующий resolve |
| `Frames.*` | 1/2/4/8/16 workers × 1/2/3 slots, два World через SceneManager, scoped writes/reparent, failed material/retry/hot reload, cancelled/failed extraction, scene unload при удерживаемых старых snapshots |
| `EcsConcurrencyScale` | 65536 узлов каждой формы, workers=1/16; миллионный star, точные результаты, packet budget и отсутствие task-per-entity |
| `EcsRunFault.ResolveAllocationFailuresReleaseTopologyAndPermitEditedHierarchyRetry` | 160 бюджетов host allocation при resolve admission/publication, release topology и повторное использование resolver после изменения родителя |

Frame-сценарий использует реальные FrameScheduler storage/recycle и immutable RenderWorld,
но завершает CPU-кадры вручную. Он не выдаёт это за проверку GPU in-flight или live Application.
Незавершённая extraction отменяется под gate родительской задачи; worker не ждёт свой пул.
Барьер в тестах cancellation/shutdown ограничен 10 секундами и управляется host, а не engine join.

## Запуск

```sh
cmake --preset ci-lite
cmake --build --preset ci-lite
ctest --test-dir cmake-build-ci-lite -R 'EcsConcurrency|EcsRunFault' --output-on-failure
```

Основной `EcsConcurrencyTests` имеет метки `small;cpp;ecs;acceptance`, scale —
`heavy;cpp;ecs;acceptance`. Оба используют стандартный TEST_SUITE bootstrap. Новый suite
автоматически входит в существующие Small/Linux и ASan+UBSan/Linux; scale — в Heavy/macOS.
Новые workflows не требуются. Общий CTest timeout: 120 сек для replay и 180 сек для scale.

```sh
cmake --preset ci-sanitizers
cmake --build --preset ci-sanitizers
ctest --test-dir cmake-build-ci-sanitizers -R 'EcsConcurrency|EcsRunFault' --output-on-failure
```

TSan запускается в отдельной сборке, без ASan/UBSan:

```sh
cmake --preset ci-lite -B /tmp/ge-ecs-tsan \
  -DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
cmake --build /tmp/ge-ecs-tsan --target GraphicsEngineEcsConcurrencyTests GraphicsEngineEcsConcurrencyScaleTests
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir /tmp/ge-ecs-tsan -R EcsConcurrency --output-on-failure
```

Используйте поддерживаемый Clang/OS runtime. Новый общий CMake-переключатель TSan не вводился.
Allocation fault использует существующий изолированный executable и thread-local allocator;
worker allocation failures дополнительно покрываются прежним `TraversalDynamicSpawnFailure...`.

## Наблюдаемость и границы выводов

Тесты проверяют `GetConflicts` (domain, scope, predecessor), terminal group/run/task states,
TraversalMetrics (visited, packets, peak, scheduling/work time), resolver recomputation и
ChangeCapture ranges. `conflict_edges`, frame workers/slots и scale packets записываются
в GoogleTest properties; для сохранения используйте `--gtest_output=json:/tmp/ecs-acceptance.json`.
В [benchmark baseline](../../benchmarks/traversal/README.md) сохранены измеренные latency,
packet budget и cold/hot/sparse transform показатели.

TaskSystem сейчас не публикует общие счётчики steals и peak live groups; ChangeTrackerStatistics
считает записи/версии, а не байты heap. Эти отсутствующие метрики не подменяются оценками.
Пик памяти scale измеряется внешним process RSS, относится ко всему executable и не равен
размеру dirty metadata. Проверка отдельных детерминированных replay не доказывает отсутствие
всех возможных гонок; результаты санитайзеров относятся к исполненным сценариям и платформе.

Требования: [Trello IMPROVE-0045](https://trello.com/c/fxjqvZkm),
[план ECS concurrency](https://app.notion.com/p/40299ff957ed8161b673c57c38d0c03e).
