# Renderer

`Renderer` owns the backend-independent render-facing boundary. Mutable simulation state remains in `NEcs::World`;
render preparation consumes only an immutable `RenderWorld`.

## Render components

`TransformComponent`, `CameraComponent` and `RenderableComponent` are high-level components stored in `World`.
`RenderableComponent` references logical mesh and material resources through typed `ResourceHandle` values and contains no
graphics-backend handles.

## Extraction

The Engine draw checkpoint is published before extraction. `IFrameRuntime::Extract()` executes after Update N and the
next Engine Update is not admitted until extraction completes.

Extraction reads `World` through its const query API and copies only render-facing data into the current
`FrameExecutionSlot`. Renderables acquire versioned `ResourceLease` values from Resources. A renderable whose required
mesh or material has no READY representation is not published into the snapshot.

The resulting `RenderWorld` contains stable render object IDs, camera data, transforms and retained resource versions.
Renderer code receives read-only spans and does not access mutable `World` for that frame.

After extraction completes, Update N+1 may overlap render work for N. Logical unload or reload after extraction prevents
new acquisitions but does not replace the versions already retained by `RenderWorld N`.

### Parallel extraction

An internal frame runtime can override `IFrameRuntime::ScheduleExtract()` and return the join handle from
`NInternal::ScheduleRenderWorldExtraction()`. The default runtime hook still calls the serial `Extract()` method.
The parallel helper enumerates render-facing component references in entity order, partitions them into chunks of
256 entries by default, and schedules chunks through the current shared `TaskContext`. Chunk size can be selected for
the workload; there is no task per entity. The serial helper remains available for small workloads and reference tests.

Every chunk depends on the scheduling task, so a partially submitted graph cannot begin reading World if scheduling
fails. Workers own separate temporary vectors. They never allocate from the frame arena concurrently. A join waits for
all chunks, checks errors in chunk order, then moves views, objects and retained resource records into the frame slot in
chunk/entity order. Worker completion order does not affect the published snapshot. A failed chunk prevents publication;
all World readers have finished before the join reports failure. The source World and Resources must outlive the join,
and World must remain read-only through it. Deterministic replay assumes identical component data and resource readiness
and versions during extraction.

Engine schedules the draw continuation after the extraction join, without blocking a TaskSystem worker. Update N+1
remains closed until that continuation starts. A failed join cancels draw and puts Engine into its failed/stopping path;
the frame is released only after extraction readers have drained. This also works with one worker.

## Frame ownership

`RenderWorld`, its containers and its resource leases belong to the `FrameExecutionSlot` generation identified by
`RenderFrameIdentity`. Non-trivial frame-owned objects are destroyed on recycle or abort before the slot arena is reset.
Retained resource versions therefore remain alive for all CPU users of the frame and are released at the frame lifetime
boundary.

`RenderWorld` also publishes frame resource use records derived from retained leases. `FrameSubmission` owns those
common records and moves them into the graphics backend submission boundary, so backend-side GPU lifetime retains the
same logical resource versions until the submission completion point retires. Extraction itself still does not expose
native backend objects, and Graphics does not depend on Resources.

## Renderer preparation

`Renderer` consumes only `RenderWorld`. It does not read mutable `World`, does not know Vulkan, and does not emit command
buffers.

The first renderer path builds backend-independent `DrawCommandData`:

```text
RenderWorld -> visibility -> RenderItems -> sort -> DrawCommandData
```

Visibility is intentionally conservative for MVP-3: every render object is visible to every extracted view. The output is
deterministic for the same `RenderWorld`; commands are ordered by view, material, mesh and object identity. Intermediate
data and the resulting render plan are allocated from the caller-provided frame memory resource, normally the current
`FrameExecutionSlot` arena.
