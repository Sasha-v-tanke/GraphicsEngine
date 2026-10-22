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

`RunSystems()` executes callbacks sequentially. `RunSystems(taskSystem)`
executes a scheduled dependency graph on the supplied TaskSystem's workers and
returns after joining this world's submitted tasks and committing structural
commands. The parallel overload requires stable schedule IDs for every system.
Its coordinator must run on a host thread; calls from any TaskSystem worker
are rejected before submission, so a worker never blocks waiting for its pool.
The World and TaskSystem must remain alive until the call returns. The caller
must not concurrently access mutable World state during execution.
An explicit `structuralOrder` controls structural merge only; it does not
reorder callbacks or ordinary component reads/writes. Systems without a
structural order cannot collect structural commands.

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
structural commit point.

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
Each callback owns its deferred context. Direct and submitted worker commands
are staged until all callbacks join. If any callback fails, all structural
commands from this invocation of `RunSystems` and unused system reservations
are discarded. Dependent tasks are cancelled; independent tasks are joined
before the original callback exception is rethrown. Ordinary component writes
already performed by callbacks are not rolled back. Previously queued external
commands remain available to an explicit commit. After a failure, callers must
prepare a new system reservation plan before retrying entity creates.
At commit and system rollback, free entity slots are ordered by index before
subsequent allocation. Returning unused reservations from different workers
therefore cannot change the identity of the next created entity.
Commit is rejected while a buffer still owns claimed create reservations and
has not been submitted or discarded. The rejected commit leaves reservations
intact so the caller can submit or destroy the buffer and retry.

## Access Contract

Systems registered with `SystemSchedule` use stable IDs and optional `Before`
and `After` constraints. Every system in a scheduled world must declare an ID;
legacy registrations retain their existing sequential execution mode.
Unknown targets, duplicate IDs and cyclic constraints are rejected before any
callback executes. Explicit constraints first define a canonical topological
order, choosing the smallest ready ID. Read/write, write/read and write/write
conflicts then add edges in that order; read/read access adds no dependency.
This policy respects explicit constraints and avoids registration order as a
hidden conflict-ordering key. `structuralOrder` remains a separate commit key.
Access declarations must accurately describe every component read or write,
including accesses performed by jobs spawned by that callback. A callback must
join its own jobs before returning. Undeclared access and mutable external
state shared by callbacks are the caller's synchronization responsibility.

`AddComponent<T>()`, `GetComponent<T>()` and query callbacks require a live
entity. Stale entities are rejected. `HasComponent<T>()` and
`RemoveComponent<T>()` return `false` for stale or missing data.
