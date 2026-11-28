# Performance Hub storage

`GraphicsEnginePerformanceHubStorage` is an optional host-only SQLite backend. It is not
linked into the GraphicsEngine SDK, editor, or runtime.

`Storage::Open(root)` owns `root/performance-hub.sqlite3`, applies explicit transactional
migrations, enables foreign keys, and creates `staging/` and `runs/`. A staged run receives a
UUID and can become either `Completed` or `Failed`; completed rows cannot be changed. `Complete`
copies bounded regular files to an isolated staging directory, validates and canonicalizes relative artifact paths,
then starts a SQLite transaction before publishing that directory by rename. Any failed transaction removes the
directory published by that attempt before releasing the writer lock. Status is rechecked under that lock,
and an existing published directory is never replaced. Recovery acquires the same lock and leaves directories
untouched on database read errors. Failed writes never alter an existing completed run. Artifact paths are always
relative to that run directory; absolute paths and `..` components are rejected.

All timestamps stored by this component are UTC ISO-8601 strings. Schema changes are only made by
numbered migrations in `storage.cpp`; a database newer than this binary is rejected.
Every open compares the v1 table and index definitions with an in-memory reference schema, including
primary keys, foreign keys and CHECK constraints; incompatible definitions are rejected rather than repaired.
The copy loop enforces 64 MiB against bytes actually read and records bytes actually written.

The `Small / Linux` CI job explicitly enables this optional module and runs
`GraphicsEnginePerformanceHubStorageTests`. Locally:

```sh
cmake --build <build-dir> --target GraphicsEnginePerformanceHubStorageTests
ctest --test-dir <build-dir> -R '^GraphicsEnginePerformanceHubStorageTests$' --output-on-failure
```
