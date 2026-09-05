# Project memory

Explicit project identity combines canonical final directory identity, stable UUID, aliases, and optional normalized Git repository identity. Never select a project merely because its continuity record is newest.

Default layout:

```text
%LOCALAPPDATA%\Forge Conductor\projects\registry.sqlite
%LOCALAPPDATA%\Forge Conductor\projects\<project-id>\memory.sqlite
```

Winsqlite3 repositories use RAII, migrations, prepared statements, busy deadlines, transactions, integrity checks, backups, explicit WAL policy, and FTS capability detection with deterministic fallback. Open project repositories use profile-bounded LRU eviction.

Implement legacy `memory_*` tools and all twelve `project_memory.*` tools with dedupe, redaction, idempotency, optimistic versions, tombstones, links, bounded pagination/response bytes, checksummed versioned export/import, preview/rollback, status, and strict project isolation.

Settings and CLI support scoped memory reset, continuity-only reset, combined reset, alias detach, export-before-reset, all-project reset, and separate legacy purge. Destructive operations require typed confirmation, audit, cache close, transaction, and verification.
