# Logging

`NLogging::Logger` создаёт синхронные структурированные записи без зависимости от Window, Vulkan или Editor. Создайте его в composition root приложения либо зарегистрируйте в существующем `ServiceManager`; глобального logger нет.

```cpp
NLogging::Logger logger;
logger.AddSink(consoleSink);
logger.Start();
logger.Log(NLogging::ESeverity::INFO, "game.load", "Scene loaded", {{"scene", "arena"}});
logger.Shutdown();
```

`LogRecord` владеет всеми строками до возврата из sink. Категория, severity, поля, timestamp, sequence и необязательные thread/task-trace contexts доступны sink. Runtime config задаёт minimum severity и полное отключение; отключённая запись не доставляется. Sink может бросить исключение: оно изолируется, а счётчик доступен через `GetSinkFailureCount()`.

Не вызывайте logger из real-time audio callback: формирование owned record может выделять память. `Shutdown()` прекращает публикацию, flushes sinks и допускает последующий `Start()`.
