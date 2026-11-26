# Performance Hub storage

`GraphicsEnginePerformanceHubStorage` is an optional host-only SQLite backend. It is not
linked into the GraphicsEngine SDK, editor, or runtime.

`Storage::Open(root)` owns `root/performance-hub.sqlite3`, applies explicit transactional
migrations, enables foreign keys, and creates `staging/` and `runs/`. A staged run receives a
UUID and can become either `Completed` or `Failed`; completed rows cannot be changed. `Complete`
copies bounded regular files to an isolated staging directory, validates relative artifact paths,
publishes that directory by rename, then commits artifact metadata and the terminal state in one
SQLite transaction. Failed writes never alter an existing completed run. Artifact paths are always
relative to that run directory; absolute paths and `..` components are rejected.

All timestamps stored by this component are UTC ISO-8601 strings. Schema changes are only made by
numbered migrations in `storage.cpp`; a database newer than this binary is rejected.
