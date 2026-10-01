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

## Mutation Phases

The MVP-3 core executes registered systems sequentially through `RunSystems()`.
The public access model already records `Read<T>()` and `Write<T>()`
declarations so MVP-4 can add parallel scheduling without changing system
registration semantics.

System callbacks may mutate `World`. Parallel conflict detection and deferred
structural changes are later features; this core keeps mutation rules explicit
and predictable.

## Access Contract

`AddComponent<T>()`, `GetComponent<T>()` and query callbacks require a live
entity. Stale entities are rejected. `HasComponent<T>()` and
`RemoveComponent<T>()` return `false` for stale or missing data.
