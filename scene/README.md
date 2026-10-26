# Scene lifecycle

`NScene::Scene` owns one ECS `World` and a descriptive name. A user can derive from Scene to hold scene-level state.
World remains the entity/component/systems container; Scene introduces no second ECS. Scene does not own Graphics,
Vulkan or the shared ResourceManager. Components retain logical resource handles through the existing Resources contract.

`SceneManager` owns registrations and selects at most one active scene. `CreateScene()` creates a basic Scene;
`RegisterScene(unique_ptr<Scene>)` accepts a populated user scene. Neither operation activates it automatically.
`Activate(handle)` selects or switches the active scene, `Deactivate()` selects none. Unloading an active scene leaves an
explicit empty state; there is no implicit fallback to another registered scene.

Registry operations run on the thread that created the manager and reject other threads. Publish activation/unload at
the application's controlled frame boundary. This foundation does not add I/O, automatic frame-loop wiring, streaming,
serialization or the async switching/readiness policy of IMPROVE-0031. Runtime SceneHandles are generational registry
identities, not persistent GUIDs or serialized Entity IDs.

Handles contain manager identity, slot and generation. Unload immediately invalidates the registration and all future
acquisitions through its handle. Slot reuse advances generation; exhausted generations retire the slot. Stale, foreign
and unregistered handles are rejected. Switching to the already active scene is idempotent.

`Acquire()` / `AcquireActive()` return shared ownership of a Scene. Capture that ownership in asynchronous World jobs,
including extraction, so logical unload cannot destroy their source World. Ownership retention does not authorize
concurrent World writes: obey ECS access declarations, structural phases and the extraction join contract. A scene lease
can outlive its registry or manager; it does not make the unloaded handle valid again.

After extraction joins, RenderWorld contains copied render-facing data and retained ResourceLeases in its frame slot.
It no longer reads Scene/World. Consequently a switch or unload can release the old Scene while its snapshot remains
valid through that slot's CPU/GPU completion. Shared Resources are not unloaded by SceneManager; their owner controls
logical unload, and frame/submission retention controls version lifetime.
