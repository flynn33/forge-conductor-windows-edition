# Legacy persistence fixtures

These SQL fixtures are immutable behavioral evidence for the Windows persistence migrations.

- `central-v3-minimal.sql` exactly reproduces the released v3 user-object shape constructed by the macOS continuity migration tests: only `schema_version` and pre-sequencing `context_handoffs`, with no explicit index.
- `central-v3.sql` retains the enriched C002+C003 lineage shape: the four core tables, pre-sequencing `context_handoffs`, and its updated-time index.
- `central-v5.sql` reproduces the complete central schema produced by `SQLiteStore.migrateLockedDatabase()` in Forge Conductor 0.9.0, including all three `context_handoffs` indexes and exactly one `schema_version = 5` row.
- `central-v6.sql` reproduces the complete Windows C006 schema and seeds one exactly recoverable working directory plus two ephemeral rows that C007 must remove rather than guess.
- `project-v1.sql` reproduces the complete non-FTS project-memory schema produced by `ProjectMemoryRepository.migrateUnlocked()`, sets `PRAGMA user_version = 1`, and seeds every durable table.

The rows use deterministic sentinel values so tests can prove that migrations preserve nullable fields, JSON payloads, relationships, tombstones, continuity state, and explicit integer keys. Do not update these fixtures to match a newer Windows schema; add a new fixture when a distinct legacy format must be covered.
