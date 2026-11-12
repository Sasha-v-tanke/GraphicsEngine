# Submission resource lifetime

Logical resource identity and native representation ownership are separate.
`ResourceManager::RequestUnload` stops new acquisitions; existing leases and accepted
GPU submissions retain their immutable representation versions. Hot replacement
publishes a new owner rather than modifying an in-flight native object.

## Recording and acceptance

`VulkanQueueSubmitInfo::ResourceUses` contains strong owners for the allocations,
views, bindings and pipeline storage referenced by the recorded commands. A raw
`VkBuffer`/`VkImageView` is not an ownership token. Allocation wrappers expose
`GetRetentionToken`; resource leases expose `GetGpuRepresentation`.

The internal `SubmissionRetirementQueue` represents one timeline owner. Before native
submit it copies and deduplicates owners by shared ownership identity and allocates
the retirement node. A prepared reservation rolls back on rejection or allocation
failure. After native acceptance, `Commit` only publishes the already allocated node;
it cannot allocate or throw. Presentation failure or later bookkeeping failure cannot
discard an accepted submission's use set.

Every accepted submission retains its own set. A shared owner survives until all
queues/submissions and remaining CPU/cache owners release it. Completion values
must advance monotonically, never wrap or be inferred from frame indices. There is
no fixed frame-delay rule. An object held only by submissions is physically destroyed
after the last relevant observed completion is collected.

## Collection and synchronization

Native submit and retirement metadata are serialized by the device's graphics-queue
host lock. `IsCompleted` and `Wait` observe completion without invoking representation
deleters, including when an observer is on another host thread. `Wait` is host-only;
TaskSystem workers must not block on GPU work.

`ReleaseCompleted` runs on the backend resource-owner lane, serialized with recording
and resource-state tracker writes. It detaches completed nodes under the queue lock
and destroys their owners after unlocking. This permits a deleter to reenter completion
observation without deadlocking. The device, allocator and resource-state tracker
must outlive every retained allocation and retirement collection.

Upload batches capture their destination allocations. Upload recycling explicitly
collects completed use sets. Graph presentation captures vertex/index allocations,
pipeline-cache ownership, descriptor-manager ownership and each bound image/sampler
owner during recording. Clear/present captures this set before native submit and
collects during owner-lane `PollCompletedFrames`.

Prepared graph assets and native pipeline/descriptor caches remain CPU owners under
their existing startup/warmup contracts. This change does not introduce automatic
cache eviction when logical Resources unload. Their eventual eviction can use the
same per-submission owner tokens; no global Resources registry mutex is introduced.

## Failure and shutdown

An ordinary failed `vkQueueSubmit2` leaves prior accepted uses unchanged and rolls
back only the unaccepted reservation. Its completion value can be retried. A confirmed
`VK_ERROR_DEVICE_LOST` closes admission and follows failure cleanup; it is never
reported as normal successful completion. Observation of device loss retains owners
until owner-lane collection/shutdown, while submission failure can clean them on the
submitting owner lane.

Host `Shutdown` closes admission and drains the native queue before dropping owners.
It distinguishes `DRAINED` from `DEVICE_LOST`. Other native drain errors preserve
all owners and throw, allowing an explicit retry before dependencies are destroyed.
The destructor fails closed if it cannot establish drain/device-loss safety: it
cannot return and silently free objects possibly still used by the device. Upload
destruction uses the same drain/failure cleanup before releasing command pools or
staging allocations. No idle wait is added to steady-state recording or polling.

These rules follow the Vulkan guarantees for
[queue-submit rejection](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit2.html)
and [resource lifetime after device loss](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html).

## Verification

CPU tests cover logical unload with published GPU representations, shared last uses,
independent timeline owners, reservation rollback, move/validation and shutdown
cleanup. An isolated allocation-fault sweep checks preflight rollback and allocation-free
commit/collection. Native tests use a real VMA buffer in two independently gated submissions,
unload it through Resources while both slots are active, then verify its retention
through the last completion and its destruction on owner collection or shutdown.
Validation is required and ERROR messages fail; unavailable environments propagate
an explicit SKIP from child to parent. Driver-status injection separately exercises
submit rollback, drain retry, device-loss cleanup, admission closure and reentrant
deleters without pretending to lose a real device containing pending GPU allocations.
