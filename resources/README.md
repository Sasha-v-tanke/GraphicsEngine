# Resources

`Resources` owns backend-independent logical resource identity and lifecycle.

This core layer intentionally stops before runtime I/O, GPU upload, descriptors, and residency policy. It provides the
stable state model that later loaders and graphics backends will extend without changing handle semantics.

## Contract

- `ResourceHandle<T>` is a typed, non-owning identifier. It does not own CPU data and is not a GPU object.
- `ResourceLease<T>` is a versioned retained CPU representation acquired only from READY state. Logical unload
  stops new acquisitions, while existing leases keep their published version alive until their owner releases it.
- `ResourceManager::TryAcquire()` is non-blocking: it returns no lease for a non-READY resource and never switches an
  existing lease to a later publication.
- `ResourceUseRecord` is a compatibility alias for the common type-erased handoff record used by frame snapshots and
  graphics submissions to keep a retained resource version alive across CPU/GPU lifetime boundaries.
- `ResourceIdentity` is the deterministic cache key for a logical resource request.
- Path-backed identities use `ResourceIdentity::FromPath`, which performs lexical normalization and stores generic
  separators without resolving the filesystem.
- Duplicate requests for the same canonical identity and type return the same handle, including concurrent requests.
- The state machine is `UNLOADED -> LOADING -> READY -> UNLOADING -> UNLOADED`, with `LOADING -> FAILED` and
  `FAILED -> LOADING` for retry.
- Loading operations have their own generation. Retry or unload invalidates stale completion publication.
- Unloading a `LOADING` resource records cooperative cancellation for that operation. Load work can query
  `IsCancellationRequested` at safe points before publishing.
- `ResourceManager` serializes state mutations and allows concurrent state/identity/payload observation through its
  public API. Callers never mutate public atomics or internal state directly.
- CPU payload is separate from future GPU representation and is visible only after `READY`.
- `Forget` is only legal from `UNLOADED` and invalidates existing logical handles through the handle generation.

## Shader artifacts

Shader resources load prebuilt SPIR-V artifacts only. The loader records explicit shader stage metadata and validates the
binary container enough for runtime ingestion: non-empty word-aligned data, SPIR-V magic, version, and bound fields.

Runtime shader compilation, reflection, and backend-native shader module creation are separate responsibilities. A failed
shader load publishes `FAILED` with resource identity and file path context in `ErrorInfo`.

## Images

Image resources load CPU pixel data through stb_image into `ImageData`.

The runtime format policy for this loader is fixed `RGBA8`: source channels are converted to four 8-bit channels and
alpha is synthesized when the source has no alpha channel. The loader does not flip images vertically; coordinate-space
or authoring-specific flip policy belongs to asset cooking or higher-level import configuration.

The image loader does not create GPU images, image views, samplers, or renderer objects. Failed image loads publish
`FAILED` with resource identity and file path context in `ErrorInfo`.

GPU image objects live in the Graphics module. `NGraphics::ImageDescriptor`, `ImageViewDescriptor`, and
`SamplerDescriptor` define the backend-independent contract for renderer/backend use: format, usage, access, lifetime,
subresource range, and sampling state are explicit, while native image, view, sampler, and descriptor handles remain
backend-private. CPU `ImageData` can feed later upload code without changing the resource-manager identity or load-state
contract.

## Meshes

`LoadMesh(ResourceManager&, ResourceIdentity, const std::filesystem::path&)` imports OBJ through tinyobjloader into
an immutable CPU `MeshData`, using the existing request/loading/ready/failure operation generations. This synchronous
source-import adapter performs blocking file I/O and must not run on a frame worker. Async I/O and cooked artifacts
remain separate layers; no Graphics/Vulkan dependency, GPU allocation, upload, material creation or draw occurs here.

`MeshVertex` contains `Position` / `Normal` as `std::array<float, 3>` and `Texcoord` as `std::array<float, 2>`.
`MeshData` exposes const vertex and uint32 index arrays with triangle-list topology. `GetAttributeMasks()` has one byte
per vertex: `NORMAL_PRESENT` and `TEXCOORD_PRESENT` record which attributes came from OBJ. Missing normal/UV values
are zero and their presence bits are cleared, including mixed-presence faces. Masks are CPU provenance metadata and
are separate from the vertex stream. A later upload/pipeline layer chooses its GPU layout explicitly.

Triangulation is enabled. Vertices are deduplicated by the original position/normal/texcoord index tuple (including
missing-attribute sentinels), in first-encounter order through triangulated faces. UV and normal seams remain distinct;
hash-table iteration never determines output order. Coordinates, normal magnitudes, winding and UV orientation are
preserved. No normal/tangent/UV generation, coordinate conversion or texture flip is performed.

The supported subset is `v`, `vn`, `vt` and polygon `f` records. Indices must refer to already declared attributes;
positive and relative negative indices are accepted. Positions require three components; optional homogeneous weight
and vertex colors are ignored. Normals require three components; texcoords accept one to three (missing V is zero,
optional W is ignored). Comments, UTF-8 BOM and CRLF are accepted. Line continuations are unsupported. Other directives
are ignored: groups/objects flatten into a single geometry payload in face order; smoothing, lines, points, material
assignments and `.mtl` files are not imported or opened. Submeshes and cooked mesh processing remain separate work.

Malformed/empty geometry, nonfinite/out-of-range numbers, invalid indices, discarded polygons and uint32 size overflow
publish `FAILED` with identity/path context; no partial payload becomes READY. Allocation failures are reported through
the existing error model. Duplicate READY/LOADING/UNLOADING identities return their current handle without file I/O;
FAILED identities may retry. Callers coordinate load/unload for an identity using the existing ResourceManager rules;
stale operation publication is rejected. `ResourceLease<MeshData>` keeps a published CPU version alive across logical
unload and later reload. Tests cover geometry/order/winding, tuple seams, per-vertex masks, invalid inputs, cache,
retry, retained versions, stale operations and count limits without large allocations.

## Materials

Material resources are typed CPU resources backed by `NGraphics::Material`.

Resources owns the material identity, load state, versioning, and frame-retained leases. The material object itself
contains a backend-independent pipeline identity plus immutable buffer/image/sampler binding descriptors expressed as
logical resource identities. New consumers do not acquire unloaded material versions, while already published frame
snapshots may retain their acquired material version until the frame resource records are recycled.

Native descriptor allocation and backend alignment/limit validation are not part of the Resources contract. Concrete
Graphics backends translate material bindings into their private descriptor infrastructure during submission and must
retain the resolved concrete resource dependencies for GPU lifetime.

## Resource I/O

`ResourceIo` reads raw bytes on a fixed pool of dedicated threads. `Read(path, completion)` returns a copyable,
retained `ResourceIoRequest` immediately; neither submission nor result observation performs filesystem I/O on the caller.
This service does not decode image/OBJ, mutate ResourceManager, allocate GPU objects or add polling tasks to TaskSystem.
`WorkerCount` limits concurrent reads; `MaxPendingReads` limits all accepted work including active reads and completions.
A full or stopped service rejects admission with INVALID_STATE. `MaxReadBytes` caps each result; defaults are two I/O
threads, 64 pending operations and 64 MiB per read. Retained results belong to callers and are not an eviction cache.

`GetResult()` returns null while pending, then an immutable retained result containing the path, SUCCEEDED/FAILED/CANCELLED
status and either shared bytes or contextual ErrorInfo. Empty regular files succeed with empty bytes. Missing/nonregular
files, read errors and oversized data fail without publishing partial bytes. The filesystem implementation reads in chunks
and rejects nonregular files before opening them; it cannot prevent external path replacement or interrupt a blocked OS
call. Paths therefore refer to trusted ordinary assets. Optional `ReadFunction` injection provides another I/O source or a
deterministic test backend; it must be thread-safe, honor its stop token and obey the byte limit (also checked on return).

Accepted requests deliver at most one read invocation and exactly one completion invocation, including failure/cancellation.
Completion runs on an I/O thread, outside service locks, after result publication. It should only hand off retained bytes,
for example by calling `TaskSystem::Submit([result](TaskContext&) { /* parse result->Bytes */ })` on success. Decode work
then runs on a compute worker; the I/O thread never waits for it. The caller must keep TaskSystem and callback captures
alive until I/O shutdown has drained completions. No TaskSystem/Engine lifetime is implicitly owned by this service.
Completion exceptions are contained and exposed separately by `GetCompletionFailure()`; they do not revoke read results.

`Cancel()` requests cooperative cancellation and is harmless on an empty or completed handle. A queued cancellation skips
the reader entirely. Active reads stop at the backend's next safe point and discard partial bytes; a cancellation racing
terminal publication may lose to completion. Result and request handles remain valid after the service is destroyed.

`Shutdown()` rejects further admissions, requests cancellation, drains all accepted requests/callbacks and joins threads.
It is idempotent and supports concurrent owner-side calls. Destruction performs the same shutdown. Call it from the owner
thread, never a compute worker, I/O callback or cancellation handler; shutdown waits for active OS calls and completions.
Self-join from an I/O thread is rejected. Destroying the service from its own callback is forbidden. Lifecycle ordering is
stop resource producers -> ResourceIo shutdown -> drain decode work -> TaskSystem shutdown. Read/observation/cancellation
are thread-safe while the service is alive; service destruction requires external lifetime coordination.

Tests use controlled delayed readers to verify compute-worker progress, TaskSystem handoff, cancellation, queue/concurrency
limits, failure isolation, retained results and shutdown with pending work. Full async ResourceManager pipeline integration
remains IMPROVE-0025; GPU staging is FEATURE-0069.
