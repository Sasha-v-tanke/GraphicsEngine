# Создание сущности в Scene

Подключите публичные `scene/scene.h` и `ecs/entity_ref.h`. Компоненты — обычные C++ типы; Scene использует свой существующий World.

```cpp
#include <memory>

#include <GraphicsEngine/ecs/entity_ref.h>
#include <GraphicsEngine/scene/scene.h>

struct Health {
    int Current;
    int Maximum;
    void Damage(int amount) {
        Current -= amount;
    }
};

NScene::Scene scene{"Level"};
auto player = scene.CreateEntity("Player")
    .AddComponent<Health>(100, 100)
    .AddComponent<std::unique_ptr<int>>(std::make_unique<int>(7));

// В initialization/offline host-фазе. Runtime также имеет свои safe commit boundaries.
scene.CommitPending();
player.GetComponent<Health>().Damage(20);
const auto& observer = player;
const Health& health = observer.GetComponent<Health>();
```

До commit player.IsPending() == true, IsAlive() == false, а TryGetComponent возвращает nullptr. Get выдаёт NOT_READY: ссылка на queued payload не предоставляется. После commit доступные компоненты находятся в World, фасад лишь проверяет доступ к ним. Имена необязательны и могут повторяться; Transform не добавляется автоматически.

```cpp
player.RemoveComponent<Health>();
scene.CommitPending();
player.Destroy();
scene.CommitPending();
```

Не сохраняйте компонентные ссылки между commit или после callback. EntityRef не продлевает lifetime Scene. Не вызывайте CommitPending из worker/system callback. В системах соблюдайте Read/Write и scoped entity bounds; дескриптор, полученный фабрикой в callback, действует только в нём. Подробные гарантии — [модель EntityRef](../about/entity_ref.md).
