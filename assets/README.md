# Assets

`Assets` is an offline authoring layer. `AssetId` is a canonical 128-bit persistent identity and is independent of source path, content hashes, `ResourceHandle`, `TypeId`, and `SceneObjectId`.

`CreateMeta(source, meta)` is the explicit first-discovery operation. It atomically creates `source.meta` and refuses to overwrite an existing sidecar. `ReadMeta(source)` only validates and reads; it never creates metadata. The strict, deterministic text format contains `asset_id`, `importer`, `schema_version`, and lexically ordered `setting.<key>` entries. Moving or renaming a source must move its `.meta` file with it, preserving the same ID.

This module has no runtime resource, GPU, or Vulkan ownership.
