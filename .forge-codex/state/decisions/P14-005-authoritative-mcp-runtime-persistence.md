# P14-005: Authoritative MCP Runtime Persistence Boundaries

Status: Accepted

Date: 2026-08-27

## Context

The bounded MCP protocol, authority router, tool packs, invocation guard, and
client-scoped workspace recovery exist, but production `serve` composition
still lacked three truthful runtime dependencies:

- `McpToolRouter` had no durable `IAuditRepository` implementation.
- `LegacyContextContinuityService` had no production
  `ILegacyContinuitySessionSource` and therefore could not capture durable open
  runs across process restarts.
- `forge_status` read `ITelemetryService::latest()` and substituted zero
  presence and zero open sessions when no telemetry frame existed.

The last dependency also violated the phase graph. P14 depends on the durable
P08-P13 services, while Windows collectors and capacity-one telemetry delivery
belong to P17 and P18 after the manager work in P16. The macOS 0.9.0 source
reads presence and open sessions directly from SQLite for `forge_status`; it
does not use the telemetry service.

## Decision

- Add an attach-only `WindowsAuditRepository` over the composition-owned
  `WindowsCentralDatabase`. Writes are serialized with the central database,
  store the typed argument digest but never raw argument JSON, mirror the
  legacy/current timestamp and error-code columns, retain at most 10,000 rows,
  and expose at most 200 newest-first events per read.
- Make `WindowsAgentSessionRepository` implement the existing read-only
  `ILegacyContinuitySessionSource`. Its client query selects only durable
  `open`, `active`, `running`, and `started` runs, probes `maximumCount + 1`,
  and fails rather than truncating. `isOpen` and binding recovery read the
  authoritative run rows. Binding projection data may select an identity but
  cannot override authoritative goal, working directory, ownership, or open
  state, and status reads never repair the projection.
- Introduce the narrow `IForgeStatusRepository` and
  `ForgeStatusProjection` instead of reusing `Domain::ForgeSnapshot` or
  requiring `ITelemetryService`. `WindowsForgeStatusRepository` obtains the
  `client_presence` count and globally open agent-session identifiers in one
  SQLite statement. It probes 10,001 rows for the 10,000-session bound,
  validates every identifier and rejects duplicates or malformed persisted
  data.
- `McpToolPackAdapter` propagates status-repository failures. It no longer
  reports fabricated zero status when a telemetry snapshot is absent.
- Attached repositories own only their admission flag and shared database
  reference. The future `serve` root must stop protocol ingress and drain the
  router before closing attached repositories, then close the central
  database last.

## Consequences

P14 can compose audit and `forge_status` without pulling P17/P18 telemetry
forward. Full telemetry can later consume compatible persistence projections
without becoming the authority for MCP status.

This decision does not complete P14 or pass G14. The remaining production
work includes MCP presence upsert/heartbeat/removal, source-compatible legacy
and automatic continuity summaries in `forge_status`, CLI `serve`
composition, primary/fallback process fixtures, and all 53 semantic snapshots.

## Source evidence

- macOS `Application/Tools/AgentToolPack.swift` SHA-256
  `df7e04af91142c8f9d1c9f6a27acc6b7f4ddb44d9c4d1009df98035805ed19bd`
- macOS `Application/ContinuityAutomation.swift`
- `.forge-codex/instructions/plans/phases.json`
- `.forge-codex/instructions/plans/gates.json`
- `.forge-codex/instructions/specifications/MCP_PROTOCOL.md`
- `src/Persistence/Windows/Migrations/CentralMigrations.cpp`

## Implementation evidence

- `include/ForgeConductor/Persistence/Windows/WindowsAuditRepository.h`
- `src/Persistence/Windows/WindowsAuditRepository.cpp`
- `include/ForgeConductor/Persistence/Windows/WindowsAgentSessionRepository.h`
- `src/Persistence/Windows/WindowsAgentSessionRepository.cpp`
- `include/ForgeConductor/Domain/ForgeStatusModels.h`
- `include/ForgeConductor/Contracts/IForgeStatusRepository.h`
- `include/ForgeConductor/Persistence/Windows/WindowsForgeStatusRepository.h`
- `src/Persistence/Windows/WindowsForgeStatusRepository.cpp`
- `include/ForgeConductor/Mcp/McpToolPackAdapter.h`
- `src/Mcp/McpToolPackAdapter.cpp`

