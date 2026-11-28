# Измерения ECS concurrency

## Range Query и ForEach

`BM_Query/<entities>/<mode>` сравнивает legacy callback (0), новый `World::ForEach` (1)
и range (2), включая snapshot, проверяемые EntityRef и уничтожение reader. Компоненты не копируются.
`BM_SystemQuery/<mode>/<workers>` добавляет тот же обход внутри одного зарегистрированного Read-system;
измеряется wall time с scheduling/join, без создания World и TaskSystem. Это сравнение стоимости API,
а не параллельный обход одной query. Все варианты проверяют одну и ту же контрольную сумму.

Повторение: `GraphicsEngineBenchmarks --benchmark_filter='BM_(System)?Query/' --benchmark_min_time=0.05s
--benchmark_repetitions=3 --benchmark_out=/tmp/query.json --benchmark_out_format=json`.
Локальный baseline FEATURE-0084: macOS arm64, AppleClang 21, Release, медиана трёх повторений,
4096 сущностей, микросекунды wall time:

| Исполнение | Legacy | ForEach | Range |
| --- | ---: | ---: | ---: |
| Owner | 104.99 | 291.86 | 308.56 |
| 1 worker | 127.31 | 318.66 | 317.31 |
| 2 workers | 130.94 | 243.37 | 226.66 |
| 8 workers | 87.35 | 232.99 | 234.26 |

Проверяемый range на этом backend дороже прежнего callback примерно в 2–3 раза.
Это зафиксированная стоимость lifetime/access checks и snapshot адресов, а не доказательство ускорения.
Сравнение чувствительно к нагрузке машины; для регрессионного решения повторять на одинаковом окружении.

`acceptance.cpp` расширяет существующий Google Benchmark target, не вводя другой runner.
Tree fixtures и независимые эталоны общие с [acceptance-тестами](../../tests/ecs_concurrency/README.md).
`BM_EcsAcceptanceTraversal` сравнивает runtime `RunSerial` (workers=0) с неблокирующим
TaskGroup traversal (workers=1/2/4/8/16). Создание World и пула, эталон и проверка результата
вынесены за measured interval. Внутри — topology acquisition, prepare, scheduling и host join.
Оба варианта удерживают результат до остановки таймера; destruction не включён выборочно
только в последовательный вариант. Параметры: shape / nodes / workers / work iterations.

Формы: 0 balanced, 1 star, 2 bamboo, 3 seeded random, 4 skewed. Размеры: 1024, 16384,
1000000; стоимость узла — 0 или 1024 итерации целочисленного mix. На каждой итерации
проверяется sentinel независимого эталона; полный exact-once/reference check живёт в tests.
Bamboo не имеет независимых ветвей и должен сохранять один packet при любом числе workers.

`BM_EcsAcceptanceTransforms` измеряет resolver для 1024/16384 узлов balanced tree и
1/2/4/8/16 workers. Профили: 0 cold cache, 1 hot cache, 2 leaf edit, 3 root edit.
Изменение local matrix исключено из времени; closure/подготовка/cache publication и host
ожидание включены. Счётчики resolved/recomputed/cache_hits характеризуют реальную работу,
а не предполагаемое ускорение. Полный required set остаётся O(N), даже на hot кадре.

## Масштабирование typed systems (FEATURE-0085)

`BM_TypedSystemQuery/<entities>/<workers>` измеряет полный run одного typed Read-system:
подготовку resolved batch, построение индекса, range Query и завершение run.
Размеры: 1024, 4096, 16384, 65536; workers: 1, 2, 8.
На каждой итерации проверяется контрольная сумма; создание World и TaskSystem вне измерения.
Для проверки роста стоимости сравнивайте `real_time` при фиксированном числе workers.
Фактические результаты требуют запуска benchmark; теоретическая сложность индекса не заменяет измерения.

```sh
cmake-build-benchmark/GraphicsEngineBenchmarks --benchmark_filter=BM_TypedSystemQuery \
  --benchmark_min_time=0.05s --benchmark_repetitions=3 \
  --benchmark_out=/tmp/typed-query.json --benchmark_out_format=json
```

## Повторение

```sh
cmake --preset benchmark
cmake --build --preset benchmark
cmake-build-benchmark/GraphicsEngineBenchmarks --benchmark_filter=BM_EcsAcceptance \
  --benchmark_min_time=0.05s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
  --benchmark_out=/tmp/ecs-acceptance.json --benchmark_out_format=json
```

Для более короткого сравнения исключите миллионные случаи через filter:
`BM_EcsAcceptanceTraversal/[0-4]/(1024|16384)/`. Все параметры уже зарегистрированы;
не меняйте seed или workload при сравнении. Сохраняйте JSON и metadata машины/компилятора,
используя существующий `benchmarks/run.sh`, либо явные `--benchmark_context` при прямом запуске.
Сравнивайте `real_time`: для UseManualTime это измеренная wall latency. `cpu_time` отражает
host coordinator и не включает работу всех workers; существующий `compare.py` сравнивает
именно cpu_time и сам по себе не является сравнением этой latency.

## Baseline — 16.11.2026

Apple M3 Pro, 12 logical CPUs, 18 GiB RAM; macOS 27.2.1 (26B131), Apple clang 21.0.0,
Release -O3, Google Benchmark 1.9.5 release. 16 workers означает oversubscription этой машины.
Медиана трёх повторов; affinity/frequency не фиксировались, машина использовалась как обычный
рабочий компьютер. Поэтому результат не является универсальным порогом или обещанием speedup.
Исходный benchmark code: `a239e45`; последующие изменения касаются тестов/документации.
Полная матрица и вариация сохранены в [acceptance_baseline.json](acceptance_baseline.json).
В отдельных миллионных случаях коэффициент вариации достигает 36.86%: эти медианы
особенно чувствительны к загрузке машины и требуют повторения перед сравнением регрессий.

16384 узла, 1024 mix iterations на узел, миллисекунды:

| Форма | RunSerial | 1 worker | 16 workers | Serial / 16 | Packets / peak |
| --- | ---: | ---: | ---: | ---: | ---: |
| balanced | 59.482 | 47.308 | 5.767 | 10.31× | 45 / 32 |
| star | 50.147 | 49.130 | 5.705 | 8.79× | 61 / 32 |
| bamboo | 47.011 | 49.068 | 39.202 | 1.20× | 1 / 1 |
| random | 52.744 | 55.238 | 6.325 | 8.34× | 41 / 29 |
| skewed | 52.179 | 42.563 | 5.686 | 9.18× | 61 / 32 |

У bamboo нет algorithmic parallelism: разница времени при одном packet обусловлена
условиями исполнения/измерения, а не распараллеливанием цепочки. Дорогие широкие нагрузки
показывают полезный выигрыш. Single-worker дорогой вариант близок к serial baseline, но
для tiny 1024-node workloads очередь добавляет примерно 12–20 мкс: RunSerial 21–30 мкс,
queued one-worker 33–50 мкс. Это зафиксированное ограничение, а не «нулевой overhead»;
для короткой host-owned операции доступен явный RunSerial. Выбирать его внутри worker
вместо разрешённой асинхронной операции без проверки access/phase contract нельзя.

TransformResolver, 16384 required nodes, миллисекунды:

| Профиль | 1 worker | 16 workers | Recomputed / cache hits |
| --- | ---: | ---: | ---: |
| cold | 4.345 | 6.341 | 16384 / 0 |
| hot | 3.629 | 5.281 | 0 / 16384 |
| leaf edit | 3.648 | 5.359 | 1 / 16383 |
| root edit | 5.234 | 7.101 | 16384 / 0 |

На этой дешёвой matrix-нагрузке 16 workers медленнее одного; автоматическое преимущество
parallel resolve не доказано. Эти данные сохраняют известный baseline и показывают
оставшуюся стоимость closure/maps/publication при полном required set. Переписывание
resolver/storage ради этих измерений не входит в verification-задачу.

Scale-тест до миллиона узлов прошёл Debug, ASan/UBSan и TSan на указанной машине.
Измеренный `/usr/bin/time -l` maximum RSS всего Debug scale-процесса — 611205120 байт
(около 583 MiB). Он включает fixtures, эталон и runtime allocations, не только ECS storage.
