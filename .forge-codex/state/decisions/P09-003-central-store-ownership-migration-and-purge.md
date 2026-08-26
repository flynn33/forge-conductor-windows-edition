# P09-003: Central Store Ownership, Migration, and Purge

Status: Accepted

Date: 2026-08-26

## Context

Legacy memory is global application state, not project memory. The macOS source
stores it in the central `memory_notes` table alongside agent-session,
continuity-pointer, presence, and audit data. P07 admitted the exact macOS
central-v5 shape and migrated it additively to Windows central-v6 without
changing `memory_notes`. P09 must prove that existing notes survive that path
and must provide the explicit destructive reset required by governance.

A general import from a separate macOS installation involves discovery,
hostile-input handling, conflict policy, and bulk migration beyond the owned
P09 table boundary. That work is assigned to P23. P09 can establish runtime
compatibility without prematurely creating a second importer.

## Decision

- The authoritative Windows legacy-memory store is the P07 central database
  `store.sqlite`. `memory_notes` retains the source columns and meaning:
  `key`, `body`, `tags_json`, `created_at`, and `updated_at`. P09 adds no schema
  migration and creates no per-project or sidecar memory database.
- The Windows legacy-memory repository is the sole P09 mutation boundary for
  `memory_notes`. Agent and continuity code must use a typed application
  contract for their system pointer rows instead of issuing SQL directly.
- Reuse the immutable source-compatible central-v5 migration fixture exactly as
  checked in, including its representative user note and fixed timestamps;
  migrate it through the P07 central-v6 path and prove byte-preserving
  key/body/tag storage, timestamp preservation, and idempotent reopen. Inject
  system-key-family and malformed-tag adversarial rows only into isolated test
  databases after schema creation. This is in-place compatibility evidence,
  not a bulk-import claim.
- P23 owns discovery and bulk import of external macOS stores, source-path
  handling, duplicate/conflict policy, hostile database admission, progress,
  and recovery. P09 neither scans a macOS home nor copies an external database.
- The destructive confirmation is exact and case-sensitive:
  `action='purge_legacy_memory'`, `scope='legacy-global-memory'`, and
  `token='PURGE LEGACY GLOBAL MEMORY'`. Any mismatch fails before opening a
  mutation transaction and deletes nothing.
- A confirmed purge starts one bounded `BEGIN IMMEDIATE` operation on the
  central database. In that transaction it counts the rows, deletes every row
  from `memory_notes` including `agent_run/`, `agent_active/`, and
  `continuity/` rows, verifies that the table count is zero, appends one
  sanitized successful audit event, and commits. Any delete, verification,
  audit, cancellation, deadline, or commit failure rolls back the deletion.
- The atomic audit row identifies `purge_legacy_memory`, the global scope, UTC
  occurrence time, mutating status, and success. It never stores the
  confirmation token or any deleted key, body, tag, or other note content.
  P14's request-level audit owns client and correlation identity and any
  argument digest; its sanitized representation may cover only action and
  scope, never note content or the confirmation token.
- Purge returns the transaction's deleted-row count. Repeating a confirmed
  purge is successful with count zero and still creates one sanitized audit
  record, so reset automation can be idempotent and observable.
- Purge affects no other central table. It is not an uninstall or data-retention
  default. Settings and CLI exposure may call this typed operation in their
  owning phases, but neither may weaken or pre-fill the confirmation triple.

## Consequences

Existing macOS central-v5 notes remain first-class Windows data without a
destructive reshape, while P23 retains clear ownership of cross-installation
bulk import. Normal reads and writes have exactly one central-store authority.

The explicit purge is all-or-nothing, includes hidden system rows, leaves other
central state intact, and produces a content-free audit trail. Continuity or
agent pointers removed by an intentional purge must be recreated by their
owning services rather than exempted from the requested scope.

## Evidence basis and source hashes

- `a5c7ec5750be9c5342dbc9fe5c1adde8e6c5a1f57d3009681b5ac1fb751f5ca0`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift`
- `cc6fb5cce18ac243fae179f66535bfc6ffc173860e3935d240aa64fa815a821a`
  — `.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/MemoryToolTests.swift`
- `2e5f772a59b21e8a71c3562813732e6e0cb0a0932669ce72a3bbfcebbf4cb08b`
  — `.forge-codex/state/decisions/P07-002-source-compatible-schema-lineage.md`
- `0dbe819813dc1e586b969e255bbce1047bdc70e31a476f8f83fe3f036c9e0ce4`
  — `src/Persistence/Windows/Migrations/SchemaMigrator.cpp`
- `a0a95a04c2d166994a6f0338ce9f7b5b287d0fa632cfa41998bf01d43af4226b`
  — `tests/Persistence/CentralMigrationTests.cpp`
- `a2b1e5eaf813b1f52ca4c30cd6e1977a054cb416d46b58ff12f13e4cfbbfd519`
  — `.forge-codex/instructions/architecture/PERSISTENCE_AND_MIGRATION.md`

