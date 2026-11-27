# Logging

`Logging` — backend-neutral synchronous журналирование для CPU-кода. `Logger` принадлежит приложению или зарегистрированному сервису; глобального экземпляра и service locator нет.

`LogRecord` владеет сообщением, категорией, полями и контекстами. Перед вызовом sink запись получает timestamp и монотонную sequence. Sink вызывается без внутреннего lock; исключение sink изолируется и увеличивает счётчик ошибок. Отсутствие sink допустимо. `DisabledCategories` отключает категории до создания owned record в `Log()`.

Для compile-time отключения применяйте `GRAPHICS_ENGINE_LOG_COMPILED_MINIMUM_SEVERITY` (0–5) и `GRAPHICS_ENGINE_LOG(...)`: выражения категории и сообщения ниже порога не вычисляются. Измерения разделены в `benchmarks/logging/disabled.cpp`: `BM_LoggingRuntimeDisabledCategory` для runtime category и `BM_LoggingCompileTimeDisabledSeverity` для compiled-out severity.

Состояния: `CREATED -> RUNNING -> SHUTDOWN -> RUNNING`. `Shutdown()` закрывает admission, ждёт всех ранее принятых публикаций, затем вызывает финальный `Flush()` каждого sink. `Shutdown()` нельзя вызывать из callback собственного sink: он ожидает завершения этой публикации. Snapshot sinks immutable; удалённый sink живёт до конца уже принятой публикации и уничтожается вне mutex. Путь создаёт owned строковые данные, поэтому его нельзя вызывать из real-time audio callback.
