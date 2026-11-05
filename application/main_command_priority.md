# Приоритет main-команд

MainExecutor хранит origin `(RunGeneration, FrameSequence)` для команд из frame-bound TaskContext. При одновременном ожидании сначала выбирается меньший FrameSequence; одинаковый origin сохраняет порядок публикации. Индекс физического слота в сортировке не участвует. Origin — только метаданные, без права доступа к World и без продления жизни кадра.

Engine устанавливает origin при публикации OnUpdate и draw/extraction root. TaskContext::Spawn и обычный TaskSystem::Submit переносят текущий origin; scope восстанавливается при выходе, включая исключения. Независимые RunAsync и coroutine resume явно исполняются UNFRAMED, даже если инициированы внутри OnUpdate. TaskContext нельзя сохранять за пределами invocation.

UNFRAMED FIFO получает обслуживание максимум после восьми последовательных frame commands. Счётчик сохраняется между Drain batches; при выборе UNFRAMED сбрасывается. Восемь команд ограничивают задержку фоновой публикации, сохраняя преимущество кадровой работы. Длительность отдельного callback не ограничивается: RUNNING не вытесняется. Window events/close, Stop и maintenance остаются вне этой очереди; Drain сохраняет ограничение batch.

Для первой реализации выбор выполняется линейным проходом стабильной очереди под mutex, без allocations на dispatch. Это сохраняет FIFO среди равных приоритетов и существующее уничтожение captures вне mutex. При больших очередях можно заменить индекс очереди без изменения policy. Admission generation MainExecutor отклоняет старые публикации; Stop удаляет ожидающие commands, restart обнуляет fairness и диагностику.

GetDiagnostics возвращает snapshot: peak depth, dispatched count, last origin, максимальный queue age и суммарное время Invoke producers до completion. Время включает callback, ожидание dispatch и пробуждение producer; не является чистой стоимостью scheduler. Benchmark MainCommandLatency публикует смешанные frame/background команды и выводит queue_p95_us/queue_p99_us. Это синтетическая queue latency, не гарантия частоты кадров или пользовательского UI.
