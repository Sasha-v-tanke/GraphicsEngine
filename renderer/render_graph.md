# RenderGraph

`RenderGraphBuilder` is the CPU-only frame orchestration boundary between renderer preparation and Graphics execution.
`Compile()` publishes an immutable, self-contained declaration snapshot; it never records commands or submits GPU work.
The public header is usable in a build with Vulkan and GLFW disabled.

## Declaring a frame

Create one builder for a live `RenderFrameIdentity`. Import persistent Graphics image/buffer handles with explicit initial
and final `EResourceUsage`. Metadata comes from the Graphics registry, including owner/generation validation; callers cannot
substitute arbitrary descriptors for live handles. Imports are non-owning. A duplicate import of the same resource fails
instead of hiding hazards behind aliases. IDs belong to the builder and cannot be reused by a new builder/frame slot.

The WSI integration owner supplies a `SwapchainGraphTarget` for each acquired image generation with a nonzero, unique
opaque identity and actual format/extent/sample/capability metadata. Its initial usage is `UNDEFINED` (explicitly discard
contents) or `PRESENT` (preserve previous contents); terminal `PRESENT` is mandatory and is inserted by `ImportSwapchain`.
The target is a CPU integration token, not a second presentation loop or a native image handle. WSI owns the canonical
identity-to-native-image mapping and acquisition synchronization. The same underlying image must not have two live
identities. Invalidate the token on resize/release and create a new token for the next acquisition. Graphs hold weak
references; dropping or invalidating the target rejects subsequent compile/record validation.

`GraphicsPassDescription` declares reads/writes, color/depth attachments and copied `DrawCommandData` packets. Existing
`RenderPlan::GetDrawCommands()` can be copied into `DrawCommands`. No mutable World, RenderWorld or backend objects are
passed to execution callbacks. Attachment metadata is resolved from imports, avoiding contradictory format/extent copies.
Load/store/clear intent is explicit; `LOAD` reads existing contents and `DISCARD` store invalidates them.

## Compilation and inspection

Resources are versioned in declaration order at whole-image/buffer granularity. Reads depend on the preceding writer;
writes depend on that writer and all intervening readers (RAW/WAW/WAR). Two reads do not create an edge. Duplicate or
conflicting uses in one pass fail; in-place sampled/attachment feedback is outside this subset. Attachments must have
matching write declarations, compatible extents/samples and supported format/usage combinations.

Explicit edges order otherwise independent passes. An edge reversing a resource hazard produces a cycle and fails rather
than silently changing resource version semantics. Stable Kahn ordering chooses the lowest declaration ID among ready
passes. Edges are deduplicated and ordered deterministically. Read-before-produce, attachment LOAD after discard, invalid
initial/final usages and final export of uninitialized contents fail with the existing `INVALID_ARGUMENT` error model.
`UNDEFINED` is allowed only as an explicit initial discard state, never as a pass or final usage.

`CompiledGraph` exposes const spans of ordered passes, imports and dependencies. `GetResource(id)` resolves an import
without native leakage. Pass order, semantic usages and attachment policies plus initial/final usages are the execution
plan; native transitions are derived by the backend state tracker. Repeated compile is deterministic and old results
remain independent of subsequent builder changes. Scratch and snapshot storage use ordinary owned vectors in this small
CPU baseline; no arena-backed nontrivial object is leaked or silently retained through a frame reset.

## Ownership and execution

Builder, compiled graph and their storage belong to one frame slot generation. Destroy their nontrivial objects on
abort/recycle before resetting the slot arena, following the existing frame-owned-object convention. Graph data must not
be cached across frames. `ValidateResources(frame)` rejects another slot/generation and released/reused Graphics handles
or expired/invalidated swapchain sources. Executor must call it immediately before recording. The Graphics instance must
outlive every import. Resource release/resize and validation/recording are externally serialized on the owner's thread.

Graphs do not retain GPU allocations or resource leases. The frame's existing RenderWorld/resource use records retain
the draw packets' exact logical resource versions through submission completion. Imported GPU resources and swapchain
generations remain owned externally until all consuming submissions complete. Present is not GPU completion.

FEATURE-0031 consumes this plan and resolves semantic usages via ResourceStateTracker/Sync2 and attachments via
FEATURE-0066 Dynamic Rendering. It retains the existing Graphics submission and WSI acquire/present boundary. Integration
must use the same frame identity and completion protocol for all active slots; resize/out-of-date must invalidate old
imports. First Triangle and resize/MaxActiveFrames > 1 GPU validation are the joint FEATURE-0031/0066 integration gate,
not implemented or claimed by this CPU boundary change.

## Current limits and extension points

One graphics queue; imported swapchain/persistent resources; one or more graphics passes. Graphics imports currently
support the existing single-sample 2D RGBA8/BGRA8/D32 descriptors; swapchain formats include sRGB as declared by WSI.
Native device support and limits are rechecked by the executor. Resource usage vocabulary deliberately contains only the
supported MVP subset; transfers describe resource state intent, not an independent copy-command API. No transient
allocation/aliasing, culling, compute passes, queue ownership transfers or parallel secondary recording is implemented.

Future transient declarations can extend the resource registry, while lifetimes, culling, queue assignments and recording
ranges extend compile output. Direct draw packets are the current CPU fallback payload; indirect/GpuScene payloads can
extend pass work without introducing native types or replacing the declaration/compile/execution boundary.

CPU contract tests in `test/render_graph.cpp` cover determinism, hazards, explicit edges/cycles, contents, attachment
validation, import validity and frame-slot isolation. They require no GPU/window backend.
