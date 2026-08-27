# P14-007: Alpha Stdio Persistence Ownership

Status: Accepted

Date: 2026-08-27

## Context

P03-007 assigns all long-lived durable mutation ownership to the future per-user
manager and requires GUI, CLI, MCP, and session-host processes to use
authenticated manager IPC. P16 has not implemented that manager host or IPC
client. The existing manager surface is a contract only, so enforcing P03-007
literally in P14 would require a fake production manager or would leave the
required `forge-conductor serve` process nonfunctional.

The later accepted P14-005 and P14-006 decisions require the stdio composition
root to own a central database, attached audit/status/presence repositories,
the ten-second presence lifecycle, and an explicit reverse shutdown order. The
P07 persistence layer already owns cross-process SQLite locking, transactions,
recovery, and bounded repository admission. Primary and fallback stdio roles
must run as independent processes before P16 can replace their persistence
adapters with manager clients.

## Decision

- For the machine-qualified internal alpha, one `forge-conductor serve`
  process may directly open the application-owned central database and the
  project repositories needed by its single stdio connection.
- This is a narrow host-composition exception to P03-007, not a change to
  module privacy. No peer Forsetti module receives a database handle, path
  capability, repository, or mutable product-state accessor.
- Primary and fallback roles own independent connection graphs over the same
  application data root. Every mutation continues through the existing
  transactional repositories and bounded SQLite namespace/operation guards.
- A serve process performs no store replacement, backup restore, migration
  import, purge, or other quiescence-requiring maintenance while protocol
  ingress is active.
- The executable root stops protocol ingress, drains the server/router and
  tool services, stops and joins the presence heartbeat, removes its exact
  presence owner, closes attached repositories, closes project caches, and
  closes the central database last.
- The first executable slice authorizes one canonical startup workspace.
  Dynamic project adoption and project-scoped Git/shell execution remain P14
  parity work and may not be claimed from this exception.
- P16 must replace direct serve persistence ownership with versioned manager
  IPC before the final architecture is claimed. The service interfaces remain
  the substitution boundary; MCP protocol and application code do not depend
  on the temporary concrete ownership choice.

## Consequences

G14 can obtain truthful real-process primary/fallback evidence without
inventing a manager implementation or removing features. The alpha may run
more than one SQLite connection owner, but those owners use the already-tested
transaction and locking boundaries and never perform quiescence-sensitive
maintenance.

Abrupt process termination can leave a stale presence row until the future
manager-owned bounded TTL prune runs. Normal EOF and orderly shutdown remove
the complete presence identity after the heartbeat thread joins. Manager
unavailability is not yet observable because the alpha serve path does not use
manager IPC; that limitation remains explicit and is not treated as final
cross-process parity.

## Conflict resolution

P03-007 describes the final topology, while P14-005 and P14-006 specify the
only currently implementable production ownership for the required G14
process. This decision adopts the smaller temporary exception, preserves the
final manager boundary as P16 work, and rejects both a fake manager and a
nonfunctional stdio executable.

## Evidence

- `.forge-codex/state/decisions/P03-007-data-isolation-and-cross-process-ownership.md`
- `.forge-codex/state/decisions/P14-005-authoritative-mcp-runtime-persistence.md`
- `.forge-codex/state/decisions/P14-006-client-presence-and-status-parity.md`
- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `include/ForgeConductor/Contracts/IManagerServices.h`
- `include/ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h`
- `src/Hosts/Cli/CliCompositionRoot.cpp`

