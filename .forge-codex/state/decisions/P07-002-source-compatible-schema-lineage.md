# P07-002: Source-Compatible Schema Lineage

Status: Accepted

Date: 2026-08-25

## Context

The attached macOS 0.9.0 source is the higher-precedence behavioral evidence,
while the Windows package also supplies target SQL templates. Several template
tables reuse macOS names with incompatible shapes. Examples include the
integer-keyed macOS `audit_events` table versus the text-keyed template shape,
additional Windows agent-session fields, and differing project memory link and
journal columns. Replacing either schema wholesale would discard source data or
silently weaken the Windows target contracts.

The released source provides evidence for an empty central database migrating
to v5, a legacy v3 database containing only `schema_version` and the
pre-sequencing `context_handoffs` table migrating to v5, and an empty project
database migrating to `user_version=1`. It does not provide reliable released
central v1, v2, or v4 shapes. The C002+C003 lineage also defines a distinct,
enriched v3 shape that the Windows port had already admitted.

## Decision

- Treat physical database versions as implementation details distinct from
  MCP and record wire-schema versions. Keep
  `Domain::ProjectMemorySchemaVersion == 1`; use Windows project
  `PRAGMA user_version=2` after the P002 physical migration.
- Use the following central migration identifiers:
  - C001 creates the checksum-bearing `schema_migrations` ledger.
  - C002 creates the source core tables: `schema_version`, `memory_notes`,
    `agent_sessions`, `presence`, and `audit_events`.
  - C003 creates the evidenced v3 `context_handoffs` shape and updated-time
    index.
  - C004 adds and rowid-backfills `write_sequence` and creates its index.
  - C005 adds `client_id`, creates the client-sequence index, and establishes
    exact macOS v5 compatibility.
  - C006 adds the Windows target envelope as nullable or defaulted additive
    fields and companion tables where an in-place source table already owns an
    incompatible key. It does not delete, rename, or reinterpret a source
    column. `schema_version` becomes 6 only after the entire transaction and
    ledger writes commit.
- C006 includes `client_presence`, target agent-session fields
  (`project_id`, `goal`, `cwd`, and `report_json`), target handoff fields
  (`project_id`, `session_id`, `payload_json`, and `content_sha256`), and the
  additive audit envelope required to retain both source and target values.
  Legacy rows use explicit unknown/null values rather than fabricated hashes or
  goals.
- Recognize the released minimal v3 and the enriched C002+C003 v3 as two exact,
  disjoint layouts with the same physical version. For the minimal layout,
  create the four absent C002 core tables and the absent C003 updated-time
  index inside the same admitted migration transaction before applying C004
  through C006. Do not relax validation to accept arbitrary partial v3 object
  sets.
- Use the following project migration identifiers:
  - P001 creates the exact source `user_version=1` tables, constraints,
    indexes, and continuity state. Optional FTS5 tables and triggers are a
    recorded capability, not a physical-version prerequisite.
- P002 adds `schema_migrations`, the Windows `project_metadata` ownership
  envelope, and additive target indexes/columns that do not alter source
  meaning, then sets `user_version=2`. Legacy `event_journal.detail` values are
  opaque source data (the macOS implementation commonly stores content hashes),
  so P002 leaves the additive `payload_json` column null instead of presenting
  those bytes as JSON. The original `detail` column remains unchanged.
- A database without a migration ledger is adopted only after a strict table,
  column, index, and version manifest matches an evidenced empty, minimal or
  enriched central-v3, central-v5, or project-v1 fixture. Adoption records the corresponding
  migration checksums. Ambiguous shapes, malformed ledgers, checksum changes,
  and future versions fail before the first write.
- Before changing any nonempty supported legacy database, create and validate
  an online SQLite backup. Apply migrations in `BEGIN IMMEDIATE`; on any error,
  roll back the schema and ledger together. Re-running the migrator is
  idempotent and revalidates every applied checksum.
- The Windows paths are `store.sqlite` under the data root and
  `projects\<project-id>\memory.sqlite`. The later macOS importer recognizes
  `Projects/<uuid>/memory.sqlite3` as source input but never makes that spelling
  the Windows runtime path.

## Conflict resolution

This additive-superset approach resolves the material source/template conflict
using the instruction precedence: macOS behavior and data remain intact, while
the lower-precedence Windows template requirements are added without a
destructive reshape. No parity claim will treat an empty default, null, or
unknown value as data that did not exist in the source.

The migration chain deliberately makes no claim about unsupported central v1,
v2, or v4 layouts. Guessing those schemas would violate the evidence rules.

## Evidence basis

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/ContinuityTests.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/ProjectMemoryRepository.swift`
- `.forge-codex/instructions/templates/sql/001_central_store.sql`
- `.forge-codex/instructions/templates/sql/001_project_memory.sql`
- `.forge-codex/state/baseline/p02-schema-inventory.json`
- `.forge-codex/state/decisions/P02-001-correct-macos-sqlite-safeguard-inventory.md`

