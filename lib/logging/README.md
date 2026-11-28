# Logging

`Logging` — backend-neutral synchronous журналирование для CPU-кода. `Logger` принадлежит приложению или зарегистрированному сервису; глобального экземпляра и service locator нет.

`LogRecord` владеет сообщением, категорией, полями и контекстами. Перед вызовом sink запись получает timestamp и монотонную sequence. Sink вызывается без внутреннего lock; исключение sink изолируется и увеличивает счётчик ошибок. Отсутствие sink допустимо. `DisabledCategories` отключает категории до создания owned record в `Log()`.

`ILogSink` обязан синхронизировать собственное mutable состояние: `Consume/Consume`, `Consume/Flush` и `Flush/Flush` могут выполняться одновременно, в том числе через разные Logger. Обычный `std::vector` в sink нужно защищать mutex, как в `CollectingSink` тестов. `Flush()` — best effort для текущего snapshot, без барьера публикаций. Для final drain вызывайте `Shutdown()` после остановки внешних callers `Flush()`.

При входе в `STOPPING` фиксируется snapshot финального flush. `AddSink()` в `STOPPING` отклоняется с `INVALID_STATE`; после `SHUTDOWN` он настраивает следующий `Start()`. `ClearSinks()` не отменяет уже захваченный snapshot final flush.

Для compile-time отключения применяйте `GRAPHICS_ENGINE_LOG_COMPILED_MINIMUM_SEVERITY` (0–5) и `GRAPHICS_ENGINE_LOG(...)`: выражения категории и сообщения ниже порога не вычисляются. Измерения разделены в `benchmarks/logging/disabled.cpp`: `BM_LoggingRuntimeDisabledCategory` для runtime category и `BM_LoggingCompileTimeDisabledSeverity` для compiled-out severity.

Локальный замер 28.11.2026: macOS, AppleClang 21, preset `benchmark`, три повтора по 0.1 s. Runtime disabled category: CPU mean 8.06 ns/call, median 8.07 ns, stddev 0.025 ns. Compile-time disabled severity: `0.000 ns` в отчёте Google Benchmark — цикл оптимизирован, измеримой работы нет; это не численная гарантия нулевой стоимости на любой платформе. Тест `LoggerCompileTime.DisabledSeverityDoesNotEvaluateMessage` отдельно проверяет отсутствие вычисления message. Thread affinity недоступна; значения являются baseline этой машины.

Воспроизведение: `cmake --preset benchmark`, `cmake --build --preset benchmark --target GraphicsEngineBenchmarks`, затем `./cmake-build-benchmark/GraphicsEngineBenchmarks --benchmark_filter=BM_Logging --benchmark_min_time=0.1s --benchmark_repetitions=3`.

Состояния: `CREATED -> RUNNING -> STOPPING -> SHUTDOWN -> RUNNING`. `Shutdown()` закрывает admission, ждёт всех ранее принятых публикаций, затем вызывает финальный `Flush()` каждого sink. Повторный `Shutdown()` в `STOPPING` ждёт завершения той же lifecycle generation: последующий `Start()` не меняет результат ожидания. `Shutdown()` нельзя вызывать из callback собственного sink: он ожидает завершения этой публикации. Snapshot sinks immutable; удалённый sink живёт до конца уже принятой публикации и уничтожается вне mutex. Путь создаёт owned строковые данные, поэтому его нельзя вызывать из real-time audio callback.
