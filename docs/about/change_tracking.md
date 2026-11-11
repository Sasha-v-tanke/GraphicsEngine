# Change tracking

`NCommon::ChangeTracker` publishes metadata for owner-defined domains. A key contains
owner, type and generation identities. Advancing a generation retires the old key.
Registration becomes visible at the next `PublishEpoch`.

Each writer owns a `DirtySummary`. `MarkRange` records half-open logical slot ranges;
`MarkEntity` also records a generational identity. These operations have no shared
mutex or global version increment. After writers retire, the publisher supplies
their summaries to one `PublishEpoch`. Adjacent/overlapping ranges merge; the World,
changed storage, affected logical chunks and optional entity revisions receive one
stamp for that epoch. Unchanged storage keeps its stamp.

`CaptureSnapshot` pins immutable metadata. A short mutex protects the root pointer;
reading a pinned snapshot needs no publisher lock. Component writes never use this
mutex. `CaptureChanges(cursor, domain)` returns ranges and a cursor from the same
publication. Consumers keep independent cursors and acknowledge them after processing
their changes. Empty cursors request full dirty; foreign/future cursors and retired
domain generations are rejected. Snapshots do not synchronize actual payload reads.

Configured limits bound retained history, ranges, sparse chunk/entity versions and
domains. Sparse overflow promotes the domain to whole-storage dirty. Consumers behind
retained history get full dirty; an unchanged domain can still remain clean. Holding
old snapshots retains their allocations separately: release snapshots no longer used.
`GetStatistics` reports retained entry counts, not allocator byte sizes.

Stamps compare `(Era, Value)`. Value overflow advances the era, clears history and
invalidates every domain. Exhausting both fields fails closed. Failed publication
preserves the old root and sets allocation-free invalidation. Captures return full
dirty with invalid stamps until recovery publication. Concurrent invalidation cannot
be erased by publication.

Entity revisions are opt-in. Whole-storage writes and sparse fallback invalidate
precise revisions. Range-only writes invalidate intersecting revisions. `nullopt`
means use the storage version/delta; it never certifies an unchanged entity.

## ECS adapter

`ecs/internal/world_changes.h` provides `WorldChangeAccess` at owner-phase boundaries.
`Domain<T>(world, true)` enables optional generational entity revisions, including for
a future Transform component. No mandatory component fields or storage migration
are required. Tracking keys remain separate from per-run access-coordinator keys.

System admission prepares invocation-local summaries. Scoped writers track resolved
entity slots; chunk coordinates are logical World slot coordinates. Legacy declared
writers invalidate their storage. Mutable `GetComponent<T>` also invalidates that
storage; const access stays clean. Undeclared legacy callbacks invalidate all domains.
Writable declarations are conservative even when callbacks do not modify every value.

After all invoked jobs retire, success/failure/cancellation paths publish summaries
before releasing the terminal outcome. Partial in-place writes survive failure and
remain dirty. Jobs cancelled before invocation contribute no write footprint.
Structural entity/topology changes invalidate conservatively. Metadata allocation
failure cannot interrupt World cleanup: captures stay full dirty until recovery.

For manual owner-side changes, call `WorldChangeAccess::PublishEpoch` at a quiescent
boundary. Unpublished changes force full dirty captures. Both adapter capture and
publication require the owner phase outside an active run. Actual data reads still
need the existing phase/task barrier. Mutable references must not cross publication
or callback boundaries; arbitrary writes through escaped raw references cannot be
detected. Use const legacy queries for read-only consumers.
