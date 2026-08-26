# P11-002: Project Continuity Schema, CAS, and Retry Recovery

Status: Accepted

Date: 2026-08-26

## Context

The imported project-v1 partial unique index treats only the macOS spelling
`predecessorSealed` as terminal. The Windows state model uses `completed` and
`cancelled`, so a terminal Windows operation can continue occupying the one
active-operation slot. The current row also stores `retry_at` but not the phase
that a `retry_wait` operation must resume.

## Decision

- Add immutable project migration `P003` and raise the physical project schema
  version to 3.
- `P003` adds nullable `retry_resume_state`, replaces
  `idx_rollover_active_project`, and makes its exact predicate:
  `state NOT IN ('predecessorSealed','completed','cancelled')`.
- Add `ProjectVersion3` layout metadata and exact table/index validation. Empty,
  v1, and v2 databases migrate to v3 transactionally; concurrent migration,
  rollback, backup, idempotent reopen, and immutable macOS fixture admission
  remain proven.
- Extend `ContinuityOperation` with an optional persisted resume state. Entering
  `retry_wait` requires one allowed nonterminal resume state and a bounded
  `retry_at`; recovery resumes exactly that state and never infers it from
  incomplete process memory.
- Store each transition with expected-state compare-and-swap, monotonic sequence,
  exact from/to states, bounded evidence, timestamp, and checksum. A stale
  expected state returns `conflict` without mutation.
- Initial operation creation and canonical handoff insertion are one
  transaction. Checkpoint intent is a subsequent compare-and-swap transition:
  a crash can leave only an `idle` operation that already owns its complete
  handoff, so checkpoint and startup recovery can advance it idempotently.
  Successor binding updates both the operation and the durable handoff/hash in
  one transaction. Completion publishes the project active session pointer and
  terminal transition in one transaction.
- Read compatibility accepts an imported checksum derived from either the
  persisted macOS camel-case state or its canonical Windows mapping. The first
  Windows mutation writes only canonical state spelling and checksum.
- Expose lifecycle persistence through a project repository aggregate so memory
  and continuity share the same project database owner and bounded LRU entry.
  Independently opening the same project database for the two services is
  rejected because it doubles connections and weakens shutdown ownership.

## Consequences

Completed and cancelled operations no longer block later rollovers. A restart
after a retry knows the exact safe continuation point. Since P003 changes
previous migration scope, the single authoritative G11 invocation includes the
native project migration target as replacement evidence without rerunning the
entire prior gate.

## Evidence basis

- `src/Persistence/Windows/Migrations/ProjectMigrations.cpp`
- `src/Persistence/Windows/Migrations/SchemaMigrator.cpp`
- `tests/Persistence/Fixtures/project-v1.sql`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContinuityCoordinator.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/ProjectMemoryRepository.swift`
