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
