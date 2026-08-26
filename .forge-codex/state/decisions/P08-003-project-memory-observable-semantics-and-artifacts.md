# P08-003: Project Memory Observable Semantics and Artifacts

Status: Accepted

Date: 2026-08-26

## Context

The macOS source supplies the behavioral baseline for twelve project-memory
operations. Several source implementation details conflict with stronger Windows
requirements: search advertises FTS but uses deterministic `LIKE` ranking;
export silently caps at 10,000 while import uses the ordinary 50-item batch cap;
`get` can materialize 100 maximum-size bodies; and the parity inventory labels
export read-only although it creates an artifact.

## Decision

- The twelve typed operations are initialize, remember, remember_batch, search,
  get, update, forget, list_recent, link, export, import, and status. P14 owns
  their exact JSON Schema and MCP transport registration.
- Export is a write-effect operation because it publishes an artifact. The
  current write `WorkspaceAuthority` and `AuthorizedToolCall` checks are
  mandatory. The parity inventory's read-effect label is recorded as source
  drift, not implemented behavior.
- Apply the source limits already centralized in `ProjectMemoryLimits`. Use
  `payload_too_large` for source-compatible payload bounds and `limit_exceeded`
  only for Windows resource/admission bounds.
- Redact every caller-controlled persisted text field before validation, hashing,
  deduplication, event creation, or SQL. Reject private-key material. Tags and
  identifier fields are normalized first and then passed through the same
  rejection boundary. Diagnostics never include rejected input.
- Content hashes are SHA-256 over the redacted, normalized kind, title, summary,
  optional body, and sorted tags using the source unit-separator framing.
  Idempotency-key lookup precedes content deduplication. Every persisted row is
  treated as hostile on read: bounded UTF-8, identifiers, versions, timestamps,
  numeric semantics, normalization, tombstone state, and the recomputed content
  hash must all agree before a record crosses the repository boundary. A new
  tombstone receives an ID-scoped, domain-separated hash so forgetting one
  record cannot collide with a live bodyless semantic twin; legacy bodyless
  tombstone hashes remain readable during migration.
- Search preserves source-observable scoring: exact ID 1000; exact title 400;
  title substring 200; summary substring 80; body substring 30; importance x10;
  confidence x5. Order is score descending, updated time descending, ID
  ascending. FTS may accelerate only if equivalence is proved; status may report
  the maintained FTS capability, but P08 does not claim different FTS semantics.
- Recent order is updated time descending then ID ascending. Cursors are canonical
  base64 `v1:<offset>` and are encoded as one JSON string or null scalar, not a
  single-element array. Response construction stops before the selected byte
  cap and returns a next cursor. `get` is all-or-error: its deterministic
  encoded estimate must not exceed 256 KiB; it never returns a silently partial
  ID set.
- Remember-batch remains at 50 records/1 MiB and is atomic. Export/import is a
  separate snapshot contract: include every live record or fail before artifact
  publication if the bounded 32 MiB/10,000-record snapshot ceiling is exceeded.
  Import accepts the full corresponding bounded snapshot in one transaction;
  the ordinary RPC batch limit does not make an export non-round-trippable.
- Export JSON is schema-versioned, deterministic, checksummed over the canonical
  record payload, and confined to the selected project's app-owned export
  directory. Artifact canonicalization matches the macOS 0.9.0 Foundation
  sorted-key compact encoding, including escaped solidus characters. A checked-in
  source-derived 928-byte golden export freezes this byte contract and checksum.
  Export retains one reserved final byte buffer plus one bounded record at a
  time. Import first performs a dependency-free, full-syntax SAX schema probe
  over the retained bytes. The probe is independently depth-, collection-,
  token-, byte-, and cancellation-bounded and discovers envelope and direct
  record schema mismatches even when `records` precedes `schema_version`.
  Unsupported older or newer schemas therefore stop as policy before
  current-schema redaction, hashing, or exact-key interpretation. Supported
  artifacts then pass through the strict current-schema SAX state machine,
  retaining one record at a time and incrementally hashing the canonical records
  array. Preview discards each prepared record immediately; committed import
  first completes that bounded, non-mutating validation, then reparses the same
  retained bytes without repeating the completed schema probe while consuming
  records inside one immediate transaction. This preserves failure provenance:
  only the validation
  pass can classify an artifact for quarantine, and that pass carries an
  explicit closed provenance value (`ArtifactValidation`, `Policy`, or
  `Dependency`) rather than inferring origin from a public error code. Injected
  hashing and non-verdict redaction failures remain dependency failures even
  when they return `integrity_failure`; an explicit `redaction_rejected` verdict
  remains artifact validation. Database and dependency failures roll back and
  leave a valid artifact in place. UUID values requested by a rolled-back
  ingestion transaction are intentionally not rewound. Import
  retains and validates the exact regular file, rejects
  reparses/hard links/alternate streams/path escapes, duplicate keys, excessive
  depth/bytes/records, schema/checksum/project mismatch, and supports preview
  without mutation. Cross-project import requires explicit merge policy. A
  committed malformed, over-limit, integrity-failed, or redaction-rejected
  artifact is byte-revalidated and moved handle-relatively into an app-owned
  non-reparse quarantine only when that failure originated in artifact
  validation; preview, database/dependency failures, and policy/version
  mismatches never quarantine.
  Oversized committed imports quarantine the exact handle retained by the failed
  read, while corrupt-import comparison uses a bounded 1 MiB buffer. Export,
  quarantine, and stale staging artifacts share a 256-file per-project hard
  quota, and every directory inventory is capped at 1,024 entries. Because the
  artifact-store `read` boundary does not receive preview policy, an oversized
  preview retains but never mutates its exact source handle in a fixed 16-slot
  catalog per admission stripe; a later read of the same path replaces it, a
  committed quarantine consumes it, and store destruction closes all remaining
  handles.
- Export/import artifact work has bounded serialized admission spanning database
  work, file I/O, parsing, redaction, and commit. Cancellation and deadlines are
  rechecked at bounded I/O chunks, parser events, record boundaries, hashes, and
  publication. Repository close shuts admission before waiting for the current
  owner, so preview cannot outlive a completed close.
- Update uses optimistic version comparison; the source-compatible no-field
  request is a versioned touch and advances `updated_at`. Supplied title and
  summary changes are trimmed and rejected if empty both before and after
  redaction. Forget writes a tombstone and is idempotent with `not_found`. Link
  validates two active records and is idempotent. The per-project event journal
  retains the newest 10,000 rows through one transaction-bound primary-key
  range prune per mutating operation. No operation falls back to another
  project's repository.
- P08 reset covers one selected project's memory tables and the owner-only
  all-project-memory backend. Each requires an exact action/scope/token
  confirmation; each per-project mutation runs transactionally, emits a bounded
  audit/event record, verifies deletion, and closes the cached repository. The
  all-project operation iterates the bounded registry deterministically and
  reports partial failure rather than claiming an atomic cross-database commit.
  Continuity and combined reset scopes are deferred to their owning phases.

## Consequences

The Windows port preserves source-visible ordering, errors, limits, and
idempotency while closing silent truncation and unbounded-response defects. A
created FTS index is not presented as evidence of different search behavior.
Artifacts round-trip within one explicit bound and hostile input is rejected
before mutation.

## Evidence basis

- `.forge-codex/instructions/specifications/PROJECT_MEMORY_TOOL_CONTRACT.md`
- `.forge-codex/instructions/architecture/PROJECT_MEMORY.md`
- `.forge-codex/instructions/architecture/SECURITY.md`
- `.forge-codex/instructions/plans/mcp-tool-parity.json` (indices 35 through 46)
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ProjectMemoryService.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/ProjectMemoryRepository.swift`
