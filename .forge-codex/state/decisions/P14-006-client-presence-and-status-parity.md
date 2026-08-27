# P14-006: Client Presence and Source-Compatible Status Projections

Status: Accepted

Date: 2026-08-27

## Context

The authoritative P14 persistence boundary supplied audit events, open-session
identifiers, and the top-level `forge_status` counts, but three macOS-observable
behaviors were still missing:

- stdio MCP clients were not represented in the durable `client_presence`
  table;
- the `continuity` member was a placeholder project count instead of
  `ContextContinuityService.statusSummary()`;
- the `auto_continuity` member was a placeholder project count instead of the
  calling client's automation state.

Presence rows are keyed by client ID in the inherited schema. A process can
reuse that key with a different role, deployment, or process ID, so an old
heartbeat or shutdown callback must not mutate the replacement process's row.

## Decision

- Add `ClientPresenceIdentity`, `ClientPresenceRegistration`, and the narrow
  `IClientPresenceRepository`. The complete owner identity is client ID, role,
  optional deployment ID, and optional process ID.
- Add an attach-only `WindowsClientPresenceRepository` over the
  composition-owned `WindowsCentralDatabase`. Upsert preserves the original
  `first_seen_at`, replaces owner attributes and `last_seen_at`, and heartbeat
  and removal use full-identity comparison. Heartbeat time is monotonic.
- A replacement process supersedes the old identity. Subsequent heartbeat or
  removal calls from the old identity return `false`; they cannot refresh or
  delete the replacement row.
- Extend `ILegacyContinuitySessionSource` with a bounded exact global open-run
  count. `WindowsAgentSessionRepository` counts the same four durable open
  aliases used by session discovery and fails rather than truncating above the
  supplied bound.
- Add typed `LegacyContinuityStatusSummary` and
  `ContinuityAutomationStatusSnapshot` projections. The former reads latest
  and resume-ready handoffs plus the global open-run count. The latter is
  exposed through the read-only `IContinuityAutomationStatusSource` boundary.
- Make `McpInvocationGuard` the status-source owner because it already observes
  every admitted tool completion and owns the legacy per-client progress,
  persistence, block, and resume lifecycle. This remains separate from the P12
  project-continuity automation service.
- Have the authorized tool adapter emit typed canonical `path`, `cwd`, and
  authority-root observations after non-continuity dispatch. The guard never
  derives workspace state from pre-authorization wire arguments. Authorized
  roots are retained even when a dispatched tool reports failure, matching the
  macOS observation order, while progress and persistence remain success-gated.
- Give `context_get` an exclusive per-client recovery lease. The adapter reads
  the lease-stable status and rejects a stale handoff before workspace adoption;
  the guard then atomically adopts recovered packet cwd and key-file roots,
  resets block and progress counters, and retains the last handoff ID and
  implicit roots. A stale handoff cannot mutate workspace authority or inject
  recovered paths.
- Bound client continuity state with generation-tagged active leases. At 128
  active, persisting, or blocked client states, a new client receives a
  retryable limit error. Only idle, non-persisting, unblocked states are
  evictable, and every completion mutates the exact generation it reserved.
- Validate handler receipt ownership, result consistency, canonical payload
  size, UTF-8, and NUL constraints before any progress, path, recovery, or
  block mutation.
- Map those projections to the exact macOS 0.9.0 `forge_status` keys. A legacy
  continuity status read failure yields `{}` because the source uses
  `(try? statusSummary()) ?? [:]`; it does not fail the enclosing status tool.
- Preserve the existing project-scoped automation execution model without
  adding client identity or MCP state to that service.

## Consequences

P14 now has the durable repository and typed status surfaces required for a
real stdio process lifecycle. The future CLI composition root must own the
ten-second heartbeat thread, stop and join it before presence removal, and
close the central database only after every attached repository and borrowed
service has stopped.

This decision does not complete P14 or pass G14. The existing immutable
workspace authority and single-authority Git/shell adapters cannot safely
serve projects created or adopted after startup. The next executable slice
will compose one honest startup workspace; dynamic multi-project authority and
project-scoped Git/shell dispatch remain explicit parity work.

The status path is exercised through the real router, authorizer, invocation
guard, and tool-pack adapter rather than a fabricated status snapshot. The
integration fixture proves automatic handoff, blocked status, canonical root
projection, matching context recovery, and resumed retained history.

## Source evidence

- macOS `Application/Tools/AgentToolPack.swift` SHA-256
  `df7e04af91142c8f9d1c9f6a27acc6b7f4ddb44d9c4d1009df98035805ed19bd`
- macOS `Application/ContinuityAutomation.swift` SHA-256
  `7d6f200e5863b9724b11fbd8be455feb1aae092e81fb003e09aa31df89e28c11`
- macOS `Application/ContextContinuityService.swift` SHA-256
  `f1abf82ff3fd613fa675f1edf56bd2c75155b069ee42a741bbbf8c5fd2fdfca2`
- `src/Persistence/Windows/Migrations/CentralMigrations.cpp`

## Implementation evidence

- `include/ForgeConductor/Domain/ClientPresenceModels.h`
- `include/ForgeConductor/Contracts/IClientPresenceRepository.h`
- `include/ForgeConductor/Persistence/Windows/WindowsClientPresenceRepository.h`
- `src/Domain/ClientPresenceModels.cpp`
- `src/Persistence/Windows/WindowsClientPresenceRepository.cpp`
- `include/ForgeConductor/Domain/LegacyContinuityModels.h`
- `include/ForgeConductor/Domain/ContinuityAutomationModels.h`
- `include/ForgeConductor/Contracts/IContinuityAutomation.h`
- `src/Application/LegacyContextContinuityService.cpp`
- `include/ForgeConductor/Mcp/McpInvocationGuard.h`
- `src/Mcp/McpInvocationGuard.cpp`
- `src/Mcp/McpToolPackAdapter.cpp`
- `tests/Persistence/ClientPresenceRepositoryWindowsTests.cpp`
- `tests/Mcp/McpInvocationGuardTests.cpp`
- `tests/Mcp/McpToolPackAdapterTests.cpp`
- `.forge-codex/state/evidence/P14/mcp-presence-continuity-status-checkpoint.json`
