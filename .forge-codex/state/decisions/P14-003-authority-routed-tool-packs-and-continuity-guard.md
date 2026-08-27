# P14-003: Authority-Routed Tool Packs and Continuity Guard

Status: Accepted

Date: 2026-08-26

## Context

P14 must connect the exact 53-tool catalog to the application services already
qualified in P08 through P13. Catalog membership alone is insufficient: each
call must retain its caller, project, authority generation, effect, operation,
deadline, cancellation, audit, and continuity bindings through dispatch.

The macOS behavior also prevents repeated identical calls from consuming a
chat indefinitely and persists progress checkpoints before context exhaustion.
Those policies require bounded per-client state and exact cleanup when a call
is cancelled between admission and dispatch. Git and PowerShell commands must
retain their structured output even when their process exit is unsuccessful,
and filesystem reads must never produce a response larger than the MCP codec
can encode.

## Decision

- `McpExecutionContextResolver` resolves the explicit or configured default
  project through `IWorkspaceAuthority`. `McpToolAuthorizer` issues an immutable
  capability only after caller, project, authority generation, effect, and
  operation context agree.
- `McpToolRouter` requires every catalog descriptor to have exactly one matching
  handler descriptor. It caps registrations and active operations at 64,
  rejects duplicate operation identifiers, validates shell and filesystem
  grants, forwards cancellation, hashes arguments for audit rather than storing
  them, and drains active work at shutdown.
- Guard admission is paired with completion or cancellation cleanup. A deadline
  or cancellation observed after admission cannot strand one of the guard's 64
  pending slots.
- `McpToolPackAdapter` binds all 53 catalog entries across the ten source tool
  packs to injected native/application interfaces. Closed project-memory and
  lifecycle schemas reject unknown fields; legacy packs retain their source
  compatibility rules.
- `McpInvocationGuard` preserves the source loop thresholds: a soft handoff note
  on the fourth identical call and a hard block on the ninth. It caps loop
  clients at 256, continuity clients at 128, and pending calls at 64. Concurrent
  progress threshold crossings reserve one per-client persistence generation,
  so they cannot create competing checkpoints or handoffs.
- Successful progress tools feed bounded recent-tool and recent-path windows.
  Checkpoints are requested after 50 progress events or 1,800 seconds and
  handoffs after 200 events or 7,200 seconds; forced agent lifecycle events use
  the same persistence boundary.
- Lifecycle MCP calls preserve a caller-supplied idempotency key in the durable
  continuity operation. The operation identifier remains the deterministic
  fallback. Recovery from `Idle` or `CheckpointPreparing` reuses the durable
  key rather than deriving a conflicting replacement.
- The canonical continuity document carries the optional finite
  `remaining_budget_estimate` field without changing older documents.
- Nonzero, timed-out, cancelled, and unconfirmed Git or PowerShell results keep
  their bounded stdout/stderr payload and carry a typed receipt error. Oversized
  UTF-8 file reads use bounded line pages or `next_byte_offset` pages below the
  one-MiB MCP document ceiling.

## Consequences

The production protocol components now have typed authority resolution,
capability issuance, exact catalog ownership, native tool-pack dispatch,
privacy-safe audit handoff, loop protection, progress persistence, cancellation
cleanup, and bounded result envelopes. Focused C++ tests cover the catalog,
resolver/authorizer, router, invocation guard, tool-pack adapter, continuity
codec/coordinator, and shared contracts.

This checkpoint does not complete P14 or pass G14. CLI `serve` and executable
composition, real primary/fallback process snapshots, client-scoped
`context_get` workspace adoption, legacy projection receipts, authoritative
active-session/status projection, and full per-tool semantic fixtures remain
required. The MCP parity inventory must not be marked passed until those
surfaces have runtime evidence.

## Evidence

- `include/ForgeConductor/Mcp/McpExecutionServices.h`
- `src/Mcp/McpExecutionServices.cpp`
- `include/ForgeConductor/Mcp/McpToolRouter.h`
- `src/Mcp/McpToolRouter.cpp`
- `include/ForgeConductor/Mcp/McpInvocationGuard.h`
- `src/Mcp/McpInvocationGuard.cpp`
- `include/ForgeConductor/Mcp/McpToolPackAdapter.h`
- `src/Mcp/McpToolPackAdapter.cpp`
- `tests/Mcp/McpExecutionServicesTests.cpp`
- `tests/Mcp/McpToolRouterTests.cpp`
- `tests/Mcp/McpInvocationGuardTests.cpp`
- `tests/Mcp/McpToolPackAdapterTests.cpp`
- `tests/Continuity/ContinuityCoordinatorTests.cpp`
- `tests/Infrastructure/ContinuityDocumentCodecTests.cpp`
- `.forge-codex/state/commands/20260827T035549337Z-63fdd812.json`
- `.forge-codex/state/commands/20260827T035619648Z-47888d8b.json`
- `.forge-codex/state/evidence/P14/mcp-routing-toolpack-continuity-checkpoint.json`
- `.forge-codex/state/decisions/P14-001-mcp-catalog-codec-and-wire-precedence.md`
- `.forge-codex/state/decisions/P14-002-bounded-mcp-server-and-stdio-lifetimes.md`
- `.forge-codex/instructions/specifications/MCP_PROTOCOL.md`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-codex/instructions/plans/gates.json`
