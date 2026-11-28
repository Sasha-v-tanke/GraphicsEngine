# Assets

`Assets` is an offline authoring layer. `AssetId` is a canonical 128-bit persistent identity and is independent of source path, content hashes, `ResourceHandle`, `TypeId`, and `SceneObjectId`.

`CreateMeta(source, meta)` is the explicit first-discovery operation. It atomically creates `source.meta` and refuses to overwrite an existing sidecar. `ReadMeta(source)` only validates and reads; it never creates metadata. The strict, deterministic text format contains `asset_id`, `importer`, `schema_version`, and lexically ordered `setting.<key>` entries. Moving or renaming a source must move its `.meta` file with it, preserving the same ID.

This module has no runtime resource, GPU, or Vulkan ownership.

Reader and writer enforce limits of 65536 total bytes, 4096 bytes per line (excluding LF), and 256 settings. Empty values, CR/LF inside fields and `=` inside setting keys are rejected. Allocation exceptions propagate; serialization completes before creating the temporary file, whose descriptor and name are then guarded by RAII.

`CreateMeta` guarantees no replacement of a single sidecar. It does not scan other source paths: copying a sidecar preserves its ID and can introduce a duplicate. Cross-source duplicate rejection before publication/cook belongs to the authoring/discovery caller and the future AssetRegistry (FEATURE-0139). This responsibility boundary remains subject to acceptance; this codec alone does not establish global uniqueness.
