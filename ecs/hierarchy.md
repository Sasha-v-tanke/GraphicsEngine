# Иерархия Entity и TopologyEpoch

World хранит лес: каждый узел имеет не более одного родителя, упорядоченные связи детей и локальную affine-матрицу.
`EntityReference` содержит World ID и прежний generational Entity; ссылки разных World не смешиваются даже при
совпадении индекса и поколения. `GetEntityReference` только квалифицирует handle: фактическая проверка выполняется
при чтении snapshot или применении команды. Это позволяет ссылаться на запланированные deferred creates.
Старые component API с `Entity` сохраняют свой контракт; World ID обязателен для нового hierarchy API.

`AcquireTopologySnapshot()` создаёт lease, а не копию дерева. Копии snapshot, диапазоны детей и итераторы удерживают
тот же lease; их разрешено передавать workers и освобождать на другом потоке. Snapshot обеспечивает неизменность
топологии и локальных матриц. Он удерживает собственное хранилище после уничтожения World, но не components и
не право обращаться к уничтоженному World. Для работы с components нужен обычный Scene/World lifetime и доступ ECS.
Получать lease одновременно с разрешённой structural publication нельзя: acquisition возвращает INVALID_STATE.

Create/Destroy/Add/Remove/SetLocalTransform и commit отклоняются с INVALID_STATE, пока существуют leases.
Commit проверяет это **до потребления очереди**: после завершения readers его можно повторить без потери команд.
Ожидания под mutex и блокировки worker нет. Владелец операции обязан завершить обходы и освободить все leases до
controlled commit; нельзя сохранять iterator между simulation phases. Внешние callers по-прежнему обязаны соблюдать
существующий single-owner/системный доступ World. Lease не превращает все методы World в thread-safe API.

`DeferReparent(child,parent,mode)` и overload для producer-local buffer проходят существующий deterministic merge:
порядок systems, producer и публикации. Пустой parent означает root. Создание/удаление также используют существующие
команды; непосредственные Create/Destroy доступны только в разрешённой structural phase. Реализация проверяет self,
циклы, stale/cross-world ссылки и режим до изменения связей. Команда является атомарной; при ошибке успешный prefix
batch сохраняется, ошибочная команда отбрасывается, остаток остаётся pending — как у прежних structural commands.
Epoch увеличивается после каждого успешного Create/Destroy/изменения parent. No-op и ошибка epoch не меняют.
Несколько команд публикуются читателям только после выхода из commit. Переполнение epoch/World ID отвергается.

KEEP_LOCAL сохраняет локальную матрицу. KEEP_WORLD вычисляет старую мировую матрицу и новую local = inverse(parentWorld)
× oldWorld в commit, до изменения связей. Матрицы должны быть конечными и affine; singular parent и нечисловой результат
дают INVALID_ARGUMENT. Перед публикацией проверяется обратная композиция: погрешность каждого элемента не более
`1e-4 × max(1, abs(oldWorldElement))`. Если float-local не может сохранить world с такой точностью, команда отвергается
без изменения связей/epoch. Полная матрица сохраняет shear, который нельзя точно представить произвольным TRS.
`GetWorldTransform` — итеративный serial reference без кэша и скрытых записей; его стоимость линейна по глубине.
При Destroy прямые дети становятся roots с прежними локальными матрицами; поддеревья не удаляются рекурсивно.
Повторное использование слота не наследует связи или локальную матрицу.

Это foundation иерархии World. Локальные матрицы данного API пока не заменяют `NRenderer::TransformComponent` и не
меняют extraction/rendering. Их объединение с derived cache и lazy revision model относится к IMPROVE-0043;
adaptive traversal — FEATURE-0081. Здесь нет TaskGroup, per-entity mutex, subtree estimates и Scene streaming.
