# Internal scoped access scheduling

FEATURE-0080 introduces an isolated owner-side coordinator. It does not modify
World, legacy Read<T>/Write<T>, TaskSystem, user callbacks or GPU barriers.
IMPROVE-0041 will provide the ECS adapter and scoped views. Declaring a footprint
here does not make arbitrary references to mutable World safe.

## Owner and scope contract

Use one coordinator per bounded execution epoch/batch. A storage owner registers
DataDomainKey{OwnerId, TypeId, Generation} with a strong lifetime token. All access
to that mutable domain must pass through the same coordinator. The owner ensures
that the token actually pins the described storage, stable index mapping and
entity/resource identities: this scheduler cannot inspect arbitrary objects for
liveness. Structural mutations, ID recycling and chunk remapping require
quiescence and a strictly newer generation; stale/unregistered keys are rejected.
Different owners/types are independent. Generation is a lifetime discriminator,
not a way to declare two versions of the same live mutable memory independent.

CHUNK is a resolved half-open interval in the same index space as ENTITY and
ENTITY_SET, not an opaque chunk number. Sets are sorted/deduplicated and repeated
identical footprints removed before admission. Empty sets and overflowing or
empty ranges are errors. RESOURCE addresses a logical resource ID; distinct
resource IDs are independent. Cross-space overlaps and UNKNOWN use conservative
whole-domain overlap. WHOLE_STORAGE overlaps everything in that domain.
Subtree expansion belongs to a future resolver using stable topology; it can use
resolved sets/ranges or the UNKNOWN fallback without adding unsafe assumptions.

Only READ/READ is compatible for overlapping scopes. WRITE and EXCLUSIVE exclude
all overlapping access. Compatibility describes memory access only; semantic
producer/consumer ordering must be supplied as explicit operation dependencies.

## Ordering and execution

Schedule serializes admission and assigns an owner-qualified monotonic
OperationKey. For each operation, all conflicting nonterminal predecessors and
explicit prior keys become immutable edges. Conflicts include waiting operations:
a whole-storage writer waiting for readers cannot be overtaken by later readers.
Edges always point backward, so forward references, foreign keys and cycles are
rejected structurally. Parallel producers linearize at admission; callers requiring
a replay-stable producer order must submit in that order or declare dependencies.
Completion timing never reverses the order of admitted conflicts.

WAITING operations have no TaskSystem task. Progress marks ready work QUEUED under
a short metadata lock, then calls TaskSystem::Submit outside it. User callbacks
run outside all coordinator locks. No dependency wait, polling task, per-entity
mutex or executor cancellation cascade is used. With one worker, a callback can
schedule a conflicting successor and return; completion admits that successor.
An operation must finish every declared access before returning. Detached work
does not inherit its footprint. Dynamic child fan-in remains TaskGroup/adapter
work; this coordinator never assumes a spawned child completed with its parent.

## Failure, cancellation and lifetime

Pending cancellation is atomic with the QUEUED-to-RUNNING transition. Cancel on a
running or terminal operation returns false: running access cannot be revoked.
A cancelled queued wrapper may still run as a no-op; it retains only weak state
and never user work. Failure is captured as exception_ptr. Failed/cancelled
predecessors cancel downstream rather than exposing partial results. Failure also
closes new admission; recover owner data and start a new coordinator/epoch.
Already admitted disjoint work can finish. There is no rollback of in-place writes.
Submission failure is recorded as FAILED and follows the same downstream rules.

Stop is host-only: close admission, cancel pending operations, drain running
callbacks and capture retirement, then release domain tokens on the host outside
metadata locks. It never stops or WaitIdle()s the shared executor. The executor
outlives the coordinator. The host must join concurrent producers before destroying
the coordinator, and neither callback nor capture destructor may Stop/destroy its
own coordinator (self-drain). Capture destructors may otherwise query/schedule/cancel
while admission permits. No user capture or domain token is destroyed under the
metadata lock, including rejected admissions and domain replacement.

Operation status/dependency/conflict history remains available until coordinator
destruction. This baseline retains epoch history and scans it at admission and
progress; costs are quadratic in a large densely conflicting batch. It is not a
permanent unbounded World registry. End the bounded epoch after quiescence and
construct another coordinator for subsequent work; old keys are foreign. Indexed
hazard frontiers/retirement optimizations need measured workloads and are outside
this foundational card. Diagnostics expose exact domain/scopes causing each edge;
explicit dependencies are separately visible in GetDependencies.
