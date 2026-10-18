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
An explicit `structuralOrder` controls structural merge only; it does not
reorder callbacks or ordinary component reads/writes. Systems without a
structural order cannot collect structural commands. The dependency scheduler
is planned for MVP-4.
The public access model already records `Read<T>()` and `Write<T>()`
declarations so MVP-4 can add parallel scheduling without changing system
registration semantics.

System callbacks may mutate existing components directly. Structural writes
(`CreateEntity()`, `DestroyEntity()`, `AddComponent<T>()` and
`RemoveComponent<T>()`) are rejected while systems are running; callbacks must
use the deferred structural APIs instead. `RunSystems()` collects deferred
commands and applies the queue after all currently registered systems complete.
Parallel producers should collect commands in `DeferredStructuralCommandBuffer`
instances. Before workers start, callers declare every producer and its maximum
entity-create count with `BeginDeferredStructuralPhase()`. The world reserves
those identities in stable `producerOrder`, so worker completion order cannot
change entity identity. Buffers are submitted after the join point. Callers that manage
their own phases can use `ApplyDeferredStructuralChanges()` as an explicit
structural commit point. Parallel conflict detection is a later feature; this
core keeps mutation rules explicit and predictable.

Create system worker buffers inside their owning callback, before dispatching
workers. They capture that system's structural order. Commit orders system
commands by `(structuralOrder, direct-before-worker, producerOrder)`; producer
orders must be unique within each system. Standalone external buffers form a
separate phase after system commands. A worker buffer must be submitted after
its workers join and before the owning callback returns.
Buffers belong to the specific callback invocation that created them; a later
invocation cannot submit them, and external buffers cannot be adopted by a system.
Before `RunSystems()`, declare system producers with `BeginDeferredStructuralPhase()`
and set each producer's `SystemOrder` to its owning system's structural order.
Identities are reserved by `(SystemOrder, Order)` before any callback runs, and
producer orders can be reused by other systems. System callbacks create entities
through these planned buffers; the shared `DeferCreateEntity()` overload is
rejected during system execution. New plans cannot begin inside a callback.
If a callback fails,
its direct commands, submitted worker commands and unused reservations are
discarded together.
Commit is rejected while a buffer still owns claimed create reservations and
has not been submitted or discarded. The rejected commit leaves reservations
intact so the caller can submit or destroy the buffer and retry.

## Access Contract

`AddComponent<T>()`, `GetComponent<T>()` and query callbacks require a live
entity. Stale entities are rejected. `HasComponent<T>()` and
`RemoveComponent<T>()` return `false` for stale or missing data.
