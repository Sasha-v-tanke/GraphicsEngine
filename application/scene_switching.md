# Scene switching

`NRuntime::SceneRuntime` — внутренний адаптер между SceneManager и Engine, без native backend objects.
SceneManager по-прежнему владеет только сценами, внешний ResourceManager — общими ресурсами.

На owner thread вызывайте `runtime.TryBeginFrame(engine, scenes, publish)`. Он сначала резервирует
кадр через Engine; при backpressure callback не вызывается. В `publish(SceneManager&)` выполняются
Activate/Deactivate/Unload и необходимые изменения World. После успешного возврата вызовите
`engine.CompleteUserUpdate(key)` и `engine.Draw()`. Не смешивайте этот путь с legacy Engine::Update.
Не изменяйте активный World или SceneManager между публикациями, пока его Update/Extract работают.

Допуск следующего кадра закрыт до завершения читателей предыдущего World. Поэтому публикация новой
активной сцены не пересекается с их чтением, даже если несколько старых GPU-кадров остаются in-flight.
Перед dispatch адаптер захватывает Scene lease и immutable `ScenePublication` (FrameKey, SceneHandle, имя).
Update выполняет существующий ECS system run; extraction использует chunked extraction и сохраняет
readiness policy SKIP_OBJECT. Ожидания ресурса внутри этих стадий нет.

После extraction адаптер освобождает Scene lease. Draw получает только RenderWorld, RenderPlan и
ScenePublication. Consumer вызывается на worker; он должен зарегистрировать точное GPU completion
до публикации presentation intent, сохранить frame storage до completion и не читать Scene/World.
Отложенные native операции выполняются на owner thread. Drain consumer вызывается из Engine::Stop
после CPU join, перед ожиданием GPU; callback должен завершить/отменить свои intents и не бросать.
Resources и consumer должны переживать Engine::Stop. Незавершённая reservation может быть AbortFrame;
её scene lease освобождается при повторном использовании слота либо Stop.

Unload активной сцены оставляет empty state: следующий snapshot пуст, presentation может очистить
экран без draw commands. Автоматического выбора другой сцены нет. Unload не отменяет общую загрузку
и не выгружает внешние ресурсы. Старые snapshots сохраняют версии ресурсов через frame leases.
Исключение из publish отменяет reservation, но не откатывает выполненные изменения SceneManager.

`GraphicsEngineSampleSceneSwitching` переключает две сцены с общей checker texture, material и cube mesh,
повторно выгружает активную сцену и создаёт замену. Дополнительный OBJ читается из файла через
ResourceIo с управляемой задержкой, декодируется в TaskSystem и публикуется через AsyncResourceLoader.
До READY второй объект пропускается. В этом sample отдельный TaskSystem принадлежит resource service;
frame work использует executor Engine. I/O не занимает ни один из compute executors.
Shutdown resource service выполняется до остановки его executor; Engine::Stop присоединяет кадры/GPU.

Texture upload и pipeline bootstrap выполняются до запуска frame loop. Для фонового mesh используется
существующий host-visible путь GraphPresentation::PrepareMesh: owner tick не ждёт I/O или GPU upload.
Полноценное streaming размещение device-local assets остаётся задачей backend, а сериализация сцен
и автоматическое освобождение prepared backend assets здесь не вводятся. Sample использует unlit
texture shader, без освещения и depth attachment; каждый cube выпуклый, с back-face culling.

Sample печатает frame/slot, имя сцены либо empty, draw count и resource state с policy=skip.
CPU integration tests удерживают три точных GPU completion, проверяют delayed I/O, readiness snapshots,
backpressure, выгрузку исходного World до GPU completion и многократное переключение.
Vulkan heavy test проверяет workers=1/3 и MaxActiveFrames=1/2/3 с validation и точным recycle count.
