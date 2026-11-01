# Serial Vulkan graph execution (R3)

`GraphPresentation` connects immutable `DrawCommandData` to the Vulkan backend. It owns one device,
PipelineManager, prepared asset versions and the existing acquire/submit/present path. All entry points use
its constructing thread. Window and the submitting Engine must outlive their work; stop/drain Engine before
tearing down presentation. Destruction drains native GPU/WSI uses before destroying prepared resources.

## Preparing assets

`PrepareMesh` receives a move-only CPU lease, validates indexed triangle topology, allocates vertex/index buffers
through VMA and writes/flushed host-visible memory before submission. No upload wait runs on compute workers.
`PrepareMaterial` warms PipelineManager before recording; no pipeline creation occurs in a draw loop. Exact
resource handles (including owner/generation) and versions resolve draws. Unsupported or unprepared versions
fail before native recording. Prepared immutable versions remain resident until presenter destruction; eviction,
batched staging and streaming are later ownership policies, not implicit behavior of this baseline.

The vertex stream is `MeshVertex`; pipelines declare its binding/attributes. An optional vertex push constant range
of 64 bytes receives the column-major model transform from `DrawCommandData`; zero means no transform input.
Shader authors own shader/layout compatibility. The neutral pipeline descriptor/key includes the range. Camera/view
projection and material descriptor binding are not inferred from this model transform. R4 extends prepared material
binding resolution without changing `DrawCommandData` or graph submission.

## Acquired targets and execution

`Render(frame, producer)` performs nonblocking acquire. It invokes the producer only after acquiring a real image,
with a fresh backend-independent `SwapchainGraphTarget` containing its format/extent. The producer compiles that
frame's graph. A target expires immediately after recording and cannot be cached across images, retries or resize.
Before native recording, CPU validation checks frame identity, target provenance, draw versions and pipeline formats.

The R3 supported subset is one imported acquired color target and one or more ordered color passes, indexed direct
draws and immutable prepared meshes/materials. The compiler owns logical order and attachment load/store/clear data;
the executor maps them into Sync2 barriers and Dynamic Rendering. Color transitions include read/write access for
LOAD and blending, and the last transition is Present. Host writes finish and flush before the queue submission's
host-to-device domain operation. Prepared mesh buffers are immutable implicit reads of draw asset versions.

Other imported images/buffers, depth, descriptors, transient allocation, async queues and secondary recording fail
explicitly rather than being silently ignored. General RenderGraph declarations stay unchanged and may be extended
by later execution paths; this executor does not claim to implement every compiler-supported graph feature.

FrameIndex and FrameSlotIndex stay distinct from acquired WSI image identity. Slots reset only after their exact
GPU completion. NOT_READY/OUT_OF_DATE/SUSPENDED without submission consume no frame and do not invoke the producer.
After accepted submit, the result always retains completion, including a later present failure. Retain that token
before propagating failure. A recording/submit/present exception makes the native path require teardown.

## Triangle scenario and validation

Build `GraphicsEngineSampleTriangle` with `ci-vulkan-glfw` and samples enabled. Run it on the application/main thread.
It creates a World with a camera and an indexed mesh/material. One Engine controls frame admission and worker work;
its frame runtime extracts RenderWorld and prepares Renderer plans in frame storage. The main thread pumps events
and graph presentation. Each plan registers a completion future before publication; Engine keeps its storage until
GPU completion and recycles it afterward. No Vulkan or GLFW handles appear in sample code.

The sample renders 240 frames, resizes and exercises zero-size suspension, then prints submitted/completed/recycled
counts. Adjacent GLSL has checked-in SPIR-V words, reproducible with `glslangValidator -V`; no runtime shader compiler
is required. Tests run the same World/Engine path with WorkerCount/MaxActiveFrames 1 and 3 under synchronization
validation. A separate GPU test injects NOT_READY/OUT_OF_DATE and a post-submit present error while successful native
calls still execute on the real driver. CPU tests cover malformed draws, frame provenance and pipeline compatibility.
Unavailable Vulkan/display/validation is an explicit SKIP. Validation errors fail the test.
