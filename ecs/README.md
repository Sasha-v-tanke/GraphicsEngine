# ECS

`Ecs` owns mutable simulation state. It is backend-independent and does not
depend on Render, Graphics, Vulkan, Window or resource backends.

## World Ownership

`World` owns entities, component storage and registered systems. Entity handles
are generational: destroying an entity invalidates every previous handle to the
same slot, even if the slot is reused by a later entity.

Destroying an entity removes all of its components immediately. Removing a
component invalidates references to that component. Creating or removing other
entities/components does not promise stable iteration order for queries.
Deferred structural changes reserve entity identities and enqueue create,
destroy, add-component or remove-component commands. They become visible only
after the queue is committed. A pending deferred entity create that appends a new
entity slot blocks immediate entity creation until the deferred queue is
committed. A pending deferred create that reuses a free slot removes that slot
from the free list immediately, so immediate creation cannot collide with the
reserved identity.

## Mutation Phases

The MVP-3 core executes registered systems sequentially through `RunSystems()`.
The public access model already records `Read<T>()` and `Write<T>()`
declarations so MVP-4 can add parallel scheduling without changing system
registration semantics.

System callbacks may mutate existing components directly. Structural writes
(`CreateEntity()`, `DestroyEntity()`, `AddComponent<T>()` and
`RemoveComponent<T>()`) are rejected while systems are running; callbacks must
use the deferred structural APIs instead. `RunSystems()` collects deferred
commands and applies the queue after all currently registered systems complete.
Parallel producers should collect commands in `DeferredStructuralCommandBuffer`
instances, submit those buffers after their join point, and use stable
`producerOrder` values as the deterministic merge policy. Callers that manage
their own phases can use `ApplyDeferredStructuralChanges()` as an explicit
structural commit point. Parallel conflict detection is a later feature; this
core keeps mutation rules explicit and predictable.

## Access Contract

`AddComponent<T>()`, `GetComponent<T>()` and query callbacks require a live
entity. Stale entities are rejected. `HasComponent<T>()` and
`RemoveComponent<T>()` return `false` for stale or missing data.
