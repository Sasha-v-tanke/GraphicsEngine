
## First GPU Frame (MVP-2)

Set `ApplicationConfig::Presentation` to `NGraphics::PresentationConfig{}` to enable the clear presentation path.
`MaxActiveFrames` on Application is the only runtime depth setting; it overrides the presentation factory's
standalone count. Vulkan/GLFW must be enabled. CPU-only applications leave Presentation unset.

Application processes Window events and pumps Graphics around the existing Engine checkpoints.
Engine draw workers publish frame intents with their logical FrameIndex and FrameSlotIndex; they never
access Window or native handles. The application thread owns Graphics, its one surface/swapchain and
all recording/submission/completion maintenance. A bounded 1 ms maintenance wait keeps backpressure
from spinning while Window events continue to be serviced. No worker waits for WSI/GPU.

A future is registered in FrameStorage before an intent is visible to the main thread. NOT_READY,
OUT_OF_DATE and SUSPENDED retain that intent and identity without repeating user callbacks. A completion
is published immediately after native submit, before fallible presentation/bookkeeping. Post-submit
failures retain the token and promise until exact GPU completion; device loss ends it exceptionally.
Engine reaps only after CPU draw and the registered completion finish. Vulkan separately gates pool reuse
on that timeline value. Swapchain ImageIndex and per-image binary semaphore lifetime remain separate.

Run stops admission, joins CPU producers, cancels unsubmitted intents, drains accepted GPU work, then
recycles Engine slots. Graphics/presentation destruction precedes Window destruction. Drain waits at most
10 seconds for timeline progress; a timeout/device error is reported through Engine and Run. The native
backend's final device teardown remains responsible for native object safety. A failed presentation
runtime cannot be restarted. A clean Run may be restarted with fresh Engine frame identities.

`GetFrameDiagnostics()` reports submitted, completed and GPU-recycled counts plus Abandoned CPU intents
that never reached native submit. Clean stop has Submitted == Completed == Recycled; abandoned intents
are excluded from those GPU counts. Validation handlers must not throw and must outlive the Application.

The clear-only backend deliberately rejects resource creation and graph execution. The same frame intent,
completion and lifetime boundary can carry subsequent RenderGraph work without exposing Vulkan to callers.

Build and run: `cmake --preset ci-vulkan-glfw -DGRAPHICS_ENGINE_BUILD_SAMPLES=ON`,
`cmake --build --preset ci-vulkan-glfw`, then `./cmake-build-ci-vulkan-glfw/GraphicsEngineSampleClearFrame`.
The sample presents 240 colored clear frames and prints submitted/completed/recycled counters.
`GraphicsEngineApplicationPresentationTests` exercises public Application/Engine/Graphics with one and three
slots and resize, requires validation, and fails on every ERROR including teardown. Unsupported initialization
explicitly skips; runtime errors do not skip. CPU contract tests cover suspension, retry identities, worker
ordering, post-submit present failure and device loss.

Supplementary heavy lifecycle tests use the same FrameLoop/Engine/Graphics pump with controlled platform
intake: native iconify plus explicit zero extent, restore and resumed submissions, for N=1 and N=3.
They also inject NOT_READY, OUT_OF_DATE and SUBOPTIMAL at the Vulkan dispatch boundary; successful
acquire/present still call the real driver. These deterministic status cases are separate from the
unmodified public Application smoke. Both check validation through teardown and clean-stop counters;
the lifecycle cases additionally restart the clean frame loop and verify recycling again.
