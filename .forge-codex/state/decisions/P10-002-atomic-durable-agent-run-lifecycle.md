# P10-002: Atomic Durable Agent-Run Lifecycle and Reserved Projections

Status: Accepted

Date: 2026-08-26

## Context

The macOS service preserves an agent run in `agent_sessions`, an
`agent_active/<client>` memory note, and an `agent_run/<session>` memory note.
Start and completion update those records through separate operations, so an
abrupt process exit can leave a closed predecessor without a replacement, an
open run without recovery metadata, or a closed run with an active pointer.
Only reattachment uses one immediate transaction. The macOS restart tests
construct a second service cleanly; they do not terminate a process during a
mutation.

G10 explicitly requires durable lifecycle behavior and crash/restart
persistence. The Windows central-v6 schema already retains the source columns
and adds nullable `project_id`, `goal`, `cwd`, and `report_json` fields. No new
physical migration is required.

## Decision

- `AgentRunRecord` is the authoritative typed representation of one central-v6
  run. It wraps the source-compatible `AgentSession` and separately represents
  nullable target fields so imported v5 rows are not assigned invented goals,
  paths, projects, or reports.
- `IAgentSessionRepository` exposes semantic transaction operations for start,
  status touch, expected-owner reattachment, completion, stale close, recovery,
  and projection repair. The P05 `save`, `get`, and `list` methods remain during
  compatibility migration but are not used to choreograph compound P10 state
  transitions.
- `WindowsAgentSessionRepository` executes every compound mutation through the
  P07 bounded `WindowsDatabaseStore::runExclusive` seam and a single
  `BEGIN IMMEDIATE` transaction. Prepared statements update `agent_sessions`
  and only the two reserved `memory_notes` namespaces owned by the agent
  lifecycle.
- Start closes every open-status alias for the same client, inserts exactly one
  new `open` row, writes the active binding, and writes the run projection in
  one transaction. Completion stores the bounded report and summary, changes
  the row to `closed`, and removes only a matching active pointer in one
  transaction; the run projection is retained. Reattachment compares the
  expected owner, closes another open run already owned by the destination,
  transfers the row, deletes only the matching old pointer, and writes the new
  pointer atomically.
- The four source status aliases `open`, `active`, `running`, and `started` are
  open. `closed`, `completed`, and `failed` are closed. New and normally ended
  source-compatible runs use `open` and `closed` respectively.
- Repository reads are bounded to at most 10,000 rows and use a stable timestamp
  plus identifier order. Persisted identifiers, UTF-8, timestamps, statuses,
  report/projection JSON, and semantic limits are validated. Malformed hostile
  rows fail with an integrity error; current time or a default status is never
  fabricated.
- The central-v6 row is authoritative during recovery. Valid legacy projections
  are retained. Missing or stale projections are rebuilt from the authoritative
  row, persisted run projection, and current catalog in a checked transaction.
- Every operation carries the same `OperationContext` through admission,
  lock/busy waits, statements, and commit. Cancellation or expiry before commit
  rolls back. Once commit succeeds, the committed outcome is returned rather
  than reporting a false rollback.
- The service and repository introduce no background thread or timer. Their
  composition owner controls shutdown, rejects new work, drains bounded
  admitted work, clears the 128-entry in-memory binding cache, releases
  repository references, and closes the shared central database last.

## P09 boundary refinement

P09-001 and P09-003 made the generic legacy-memory repository the P09 mutation
boundary for `memory_notes` and instructed later phases not to issue SQL from
Application. P10 refines that decision for the two reserved agent namespaces:
the typed agent-session repository owns their compound central transaction.
Application still has no SQL, database handle, or cross-module dependency, and
ordinary user notes remain exclusively owned by the legacy-memory repository.
This refinement is necessary to satisfy the higher-level crash-consistency and
single-owner requirements; coordinating two independent repository calls
cannot provide the required atomic result.

## Consequences

An abrupt exit exposes either the complete previous state or the complete new
state for start, completion, and ownership transfer. Recovery can repair legacy
projections without guessing. P07 migration lineage remains unchanged and v5
rows remain readable through explicit nulls.

## Rejected alternatives

- Service-side `list/get/save` choreography: rejected because it races across
  processes and cannot make session and pointer state crash-atomic.
- A P10 sidecar database: rejected because the canonical central store already
  owns these records and upgrades must preserve existing data.
- Adding C007 solely for P10: rejected because C006 already has the required
  authoritative fields.
- Opening an unowned hidden SQLite connection: rejected because the central
  database and connection lifetime must remain explicit and bounded.

## Evidence basis

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/AgentSessionService.swift`
  — SHA-256 `9b9dc37ee186e5d195484fbabd1d5cef25f701675d1e60e082e18d06f9d0cb00`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift`
  — SHA-256 `a5c7ec5750be9c5342dbc9fe5c1adde8e6c5a1f57d3009681b5ac1fb751f5ca0`
- `src/Persistence/Windows/Migrations/CentralMigrations.cpp`
- `.forge-codex/state/decisions/P09-001-legacy-memory-service-and-repository-boundary.md`
- `.forge-codex/state/decisions/P09-003-central-store-ownership-migration-and-purge.md`
- `.forge-codex/instructions/plans/gates.json` — G10

