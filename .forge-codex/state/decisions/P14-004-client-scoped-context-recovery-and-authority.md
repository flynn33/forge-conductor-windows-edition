# P14-004: Client-Scoped Context Recovery and Workspace Authority

Status: Accepted

Date: 2026-08-26

## Context

The legacy `context_get` surface returns continuity packets whose working
directory and key-file values are untrusted input. P14 must let a new MCP chat
recover the correct registered project without retaining an arbitrary path or
silently falling back to a different project's authority. Recovery also must
not let a missing, stale, expired, or concurrently superseded handoff clear the
invocation guard's current continuity block.

The earlier MCP checkpoint resolved either explicit request metadata or one
configured default project. It did not retain a client-scoped recovered
workspace, and the router still required explicit project metadata even after
the resolver had issued a valid project authority. The earlier adapter also
reported successful projection inspection without a production projection
receipt.

## Decision

- `IMcpClientWorkspaceContext` owns one optional recovered workspace snapshot
  per MCP client. Its concrete implementation caps tracked clients and active
  adoption reservations at 128 and has explicit clear and shutdown paths.
- Recovery examines at most 1,024 registered projects, 32 aliases per project,
  and 32 packet candidates. Registry aliases and candidate comparisons have
  explicit byte budgets, and cancellation/deadline checks occur during the
  matching loops.
- Packet paths are only prefilters. A snapshot retains the canonical authority
  root returned by `IWorkspaceAuthority::authorize`; it never retains the raw
  packet path. A no-match result clears the prior client snapshot and returns a
  typed warning.
- Concurrent adoptions use ordered reservations. The latest reservation wins;
  a slower superseded request receives a retryable conflict rather than a
  response that mixes its old packet with a newer workspace.
- Explicit project metadata continues to take precedence. Otherwise the
  execution resolver uses the recovered client snapshot, reissues current
  project authority, verifies that the retained canonical root is still
  trusted, and narrows the capability to that root with a fresh generation.
- For a project-required tool without request project metadata, the router
  binds a dispatch-only copy of the request to the already-resolved authority
  project. The immutable incoming request, guard admission, and audit identity
  remain unchanged.
- A successful `context_get` carries a typed recovery receipt. The invocation
  guard validates cancellation/deadline first and atomically clears a block
  only when both the client and required handoff identifier match. It adds
  `context_budget_cleared` only after that decision.
- Until production projection inspection is composed, `context_get` reports
  `projection_checked: false`, `projection_ok: null`, `projection_status:
  "unverified"`, and no claimed projection paths.

## Consequences

A recovered continuity packet can now select the correct registered project
for subsequent project-required tools without trusting packet paths or
requiring legacy tool schemas to expose a project field. Missing context,
stale handoffs, expired completions, and superseded concurrent retrievals do
not unlock the current block. Shutdown and state growth remain bounded.

This checkpoint does not complete P14 or pass G14. CLI `serve` and executable
composition, production audit and legacy-session sources, authoritative status
and projection services, primary/fallback process semantic snapshots, and the
complete 53-tool semantic fixture set remain required.

## Evidence

- `include/ForgeConductor/Contracts/IMcpClientWorkspaceContext.h`
- `include/ForgeConductor/Mcp/McpClientWorkspaceContext.h`
- `src/Mcp/McpClientWorkspaceContext.cpp`
- `src/Mcp/McpExecutionServices.cpp`
- `src/Mcp/McpToolRouter.cpp`
- `src/Mcp/McpToolPackAdapter.cpp`
- `src/Mcp/McpInvocationGuard.cpp`
- `tests/Mcp/McpClientWorkspaceContextTests.cpp`
- `tests/Mcp/McpExecutionServicesTests.cpp`
- `tests/Mcp/McpToolRouterTests.cpp`
- `tests/Mcp/McpToolPackAdapterTests.cpp`
- `tests/Mcp/McpInvocationGuardTests.cpp`
- `.forge-codex/state/commands/20260827T044125881Z-66ec7eb1.json`
- `.forge-codex/state/commands/20260827T044230392Z-8a391847.json`
- `.forge-codex/state/commands/20260827T044823188Z-fd75371e.json`
- `.forge-codex/state/evidence/P14/client-workspace-context-recovery-checkpoint.json`
- `.forge-codex/state/decisions/P14-003-authority-routed-tool-packs-and-continuity-guard.md`
- `.forge-codex/instructions/specifications/MCP_PROTOCOL.md`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-codex/instructions/plans/gates.json`
