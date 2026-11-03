# Пакетные загрузки

`VulkanUploadManager` — внутренний backend-механизм FEATURE-0069. Он использует существующие `VulkanSubmissionManager` и resource-state transactions. Graphics queue является рабочим baseline; dedicated transfer queue остаётся необязательной оптимизацией. Публичные Resources/Renderer contracts не получают Vulkan/VMA types.

`UploadBatch(buffers, images)` копирует bytes во время вызова, записывает buffers, затем images, сохраняя порядок внутри каждого span, и возвращает один exact completion для всех операций. Destination pointers нужны во время вызова; внутренние retention tokens сохраняют allocations до completion. CPU spans после возврата можно освобождать. Повторные/перекрывающиеся назначения записываются последовательно через state tracker. Изображения сохраняют subset single-mip/layer 2D sampled color.

Пакет использует отдельную persistently mapped арену с выравниванием slices на 4 байта. Все slices этой арены принадлежат одной отправке и возвращаются вместе, только после её completion. Поэтому округлённый до nonCoherentAtomSize flush не затрагивает in-flight slices. VMA выполняет flush для non-coherent allocations и пропускает его для coherent. Для upload host invalidate не нужен: CPU читает исходные bytes, GPU читает staging; readback использует `Invalidate` отдельно.

Idle-арены и их command buffers переиспользуются. Базовый размер — 256 KiB, общий staging budget — 64 MiB, максимум 256 uploads в пакете. Большой пакет получает увеличенную арену в пределах бюджета. Для него могут освобождаться только idle-арены; при занятом бюджете возвращается ошибка `INVALID_STATE` с требованием повторить после completion. Нет скрытого ожидания, очереди с неограниченной памятью или автоматической отправки по одному asset. Размер пакета сверх бюджета/лимита, пустой пакет, неверные destinations/ranges — `INVALID_ARGUMENT` до submit.

Validate/record/flush/submit failure не публикует completion; state tracker откатывается, destinations освобождаются, idle-арена остаётся доступна для следующей попытки. Retention и память подготавливаются до submit; после успешной отправки нет throwing allocations. Владелец сериализует manager с другими операциями над тем же tracker/graphics resource state. Device, allocator, submission manager и tracker должны пережить upload manager. Shutdown ждёт его exact completions, не frame count или present.

`GetStatistics()` возвращает число/байты pending uploads и batches, staging bytes, staging/pending high-water, total submitted bytes/uploads/submissions и число созданных арен. Pending означает «completion ещё не обработан ReleaseCompleted», даже если GPU уже закончил. Прикладная публикация GPU READY относится к IMPROVE-0025: caller проверяет exact returned completion и только затем публикует ресурс для будущих consumers. Возврат из UploadBatch не означает GPU READY.

## Проверки и измерение

Heavy `VulkanUploadManager.RetainsStagingUntilTimelineCompletion` проверяет реальные copies, mixed batches, delayed completion polling, independent batch retirement, reuse, bounded memory/backpressure, rollback при failed submit, retention и shutdown. Validation ERROR приводит к падению, недоступное устройство/окружение — к явному SKIP.

Benchmark собирается с `GRAPHICS_ENGINE_BUILD_BENCHMARKS=ON`, Vulkan и GLFW. Команда: `GraphicsEngineBenchmarks --benchmark_filter=BM_UploadBatches`. Один проход передаёт 64 buffers по 4096 bytes, batch sizes 1/8/64 дают 64/8/1 submissions. Counters показывают allocations/high-water; время включает ожидание последнего completion в benchmark harness, чтобы сравнение было ограничено одинаковым объёмом работы. Такой wait не выполняется в production upload path.
