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
`FrameExecutionSlot` arena. The resulting `RenderWorld` contains stable render object IDs, camera data, transforms and
logical resource handles. Renderer code receives read-only spans and does not access mutable `World` for that frame.

After extraction completes, Update N+1 may overlap render work for N.

## Frame ownership

`RenderWorld` storage belongs to the `FrameExecutionSlot` generation identified by `RenderFrameIdentity`. Its spans are
valid only until that frame is recycled. Recycle resets the slot arena; retaining or reading a previous generation's
snapshot after recycle is invalid.

Logical `ResourceHandle` values copied into the snapshot are non-owning. Resource retention across in-flight frames is a
separate resource-lifetime concern and is not implemented by extraction.
