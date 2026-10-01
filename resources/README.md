# Resources

`Resources` owns backend-independent logical resource identity and lifecycle.

This core layer intentionally stops before runtime I/O, GPU upload, descriptors, and residency policy. It provides the
stable state model that later loaders and graphics backends will extend without changing handle semantics.

## Contract

- `ResourceHandle<T>` is a typed, non-owning identifier. It does not own CPU data and is not a GPU object.
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
