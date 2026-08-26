# P06-002: Versioned Configuration, Atomic Replace, and Recovery

Status: Accepted

Date: 2026-08-25

Amendment: `P06-006-handle-relative-atomic-publish.md` supersedes the native
`ReplaceFileW`/`MoveFileExW` linearization and ambiguous-reconciliation bullets
below. The configuration schema, recovery, bounds, publication ordering, and
cancellation decisions in this record remain accepted.

## Context

Configuration updates must preserve forward-compatible fields and cannot expose partial bytes or publish memory before durable commit. Windows replacement APIs have distinct existing-target and absent-target behavior and documented ambiguous failure states.

## Decision

- Configuration is deterministic UTF-8 JSON without BOM, with root `schema_version: 1`.
- Parsing rejects duplicate keys, malformed UTF-8/JSON, depth above 32, documents above 2 MiB, and future schema versions. Unknown root and nested fields are preserved across updates.
- A missing primary creates defaults. A corrupt primary recovers only from a valid backup. Corrupt primary plus corrupt/missing backup returns `integrity_failure`; malformed data is never silently replaced with defaults.
- Update stages and validates a complete candidate, serializes it, commits it, and only then publishes the immutable in-memory snapshot.
- Atomic replacement creates a same-directory unpredictable `CREATE_NEW` temporary file, writes in a bounded loop, calls `FlushFileBuffers`, and then linearizes.
- Existing targets use `ReplaceFileW` with flags exactly zero and an optional sibling backup. Absent targets use `MoveFileExW` with `MOVEFILE_WRITE_THROUGH`.
- `REPLACEFILE_WRITE_THROUGH`, ignore-ACL, and ignore-merge flags are prohibited.
- Temporary files are RAII-deleted on every precommit failure. A failure before the linearization point leaves the prior target and in-memory snapshot unchanged.
- Cancellation or deadline expiry before commit returns `cancelled` or `deadline_exceeded`. Cancellation observed after successful replacement returns success because the durable mutation already linearized.
- Mutable service state is serialized by a composition-owned bounded executor; no lock is held across filesystem I/O.

## Consequences

Readers observe either the old complete document or the new complete document. Backup recovery is explicit and testable, and forward-compatible fields survive Windows updates.

## Rejected alternatives

- `std::filesystem::rename` as the durability primitive: rejected because backup and Windows replacement semantics are insufficiently explicit.
- Publishing memory before disk: rejected because commit failure would split observable state.
- Silent default fallback on corrupt JSON: rejected because it destroys evidence and masks integrity failure.
- Unsafe `ReplaceFileW` flags: rejected because unsupported or ACL-ignoring behavior weakens deterministic recovery.

## Evidence

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/ConfigStore.swift`
- `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
- Microsoft `ReplaceFileW` and `MoveFileExW` API contracts
