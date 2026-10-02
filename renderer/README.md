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

## Frame ownership

`RenderWorld`, its containers and its resource leases belong to the `FrameExecutionSlot` generation identified by
`RenderFrameIdentity`. Non-trivial frame-owned objects are destroyed on recycle or abort before the slot arena is reset.
Retained resource versions therefore remain alive for all CPU users of the frame and are released at the frame lifetime
boundary.

`RenderWorld` also publishes frame resource use records derived from retained leases. `FrameSubmission` carries those
records into the graphics backend submission boundary so backend-side GPU lifetime can retain the same logical resource
versions until the submission completion point retires. Extraction itself still does not expose native backend objects.
