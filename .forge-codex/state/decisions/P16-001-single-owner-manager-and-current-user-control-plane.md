# P16-001: Single-Owner Manager and Current-User Control Plane

Status: Accepted

Date: 2026-08-27

## Context

P16 must replace the macOS launch-agent, PID-file, Network.framework dashboard,
and loopback client with one native Windows manager process. The manager is the
exclusive owner of the dashboard listener, telemetry sampling, LM Studio
reconciliation, durable continuity recovery, bounded maintenance, and published
health. The GUI and CLI attach as clients and may not create a competing
listener.

The repository already contains typed manager status, settings, patch, client,
and server contracts, plus configuration and clock abstractions. It does not
yet contain a production controller, manager executable, single-instance
lease, named-pipe protocol, current-user pipe security, dashboard listener,
startup adapter, or manager process tests.

The owner deferred security-only hardening until after alpha. Current-user IPC
authentication, loopback-only dashboard binding, and bearer-token enforcement
are observable G16 behavior and remain in scope. The deferral does not permit a
locally unauthenticated control plane.

## Decision

### Ownership and application boundary

- `ForgeConductor.Manager.exe` owns one per-user manager composition root. A
  move-only Windows lease holds a named mutex whose name includes a stable
  SHA-256-derived identifier for the current user SID.
- Creating a second lease for the same SID fails with the typed
  `OwnershipConflict` error. A stale process leaves no durable ownership marker;
  kernel object lifetime releases the lease when its final handle closes.
- A serialized application controller implements status, settings, start,
  stop, restart, repair, and shutdown behavior through injected contracts. It
  owns mutable state and exposes immutable snapshots. It performs no Win32,
  socket, pipe, filesystem, registry, or Task Scheduler work directly.
- The manager host owns ingress and shutdown ordering. Shutdown stops accepting
  pipe and dashboard work, cancels outstanding operations, stops watchdog and
  maintenance owners, releases listeners, and releases the instance lease last.

### Current-user named-pipe protocol

- The pipe name includes the same stable per-user SID hash as the instance
  mutex. The server creates it with a DACL for the current user SID and rejects
  remote clients. LocalSystem is not added unless a later concrete dependency
  requires and documents it.
- Every frame is a four-byte unsigned little-endian length followed by one
  strict UTF-8 JSON object. Both directions enforce the configured
  `namedPipeFrameBytesMaximum`, currently 2,097,152 bytes.
- Protocol version 1 envelopes contain exactly `version`, `request_id`,
  `correlation_id`, `deadline_utc_ms`, `nonce`, `method`, and `params` for
  requests. Responses contain the matching identity fields and exactly one of
  `result` or `error`. Closed envelopes reject unknown or duplicate fields,
  malformed UTF-8, excessive nesting, unsupported versions, oversized frames,
  missing identifiers, expired deadlines, and invalid nonces.
- The authentication nonce is a random 256-bit value stored with DPAPI
  CurrentUser scope. The pipe DACL is the primary identity boundary; after
  connection, the server impersonates the pipe client and verifies its token
  SID equals the manager owner SID before accepting the nonce. Nonces are
  compared without data-dependent early exit and never logged.
- Wire deadlines use UTC epoch milliseconds because a steady-clock time point
  cannot cross a process boundary. Each receiver rejects an expired or
  unreasonably distant deadline, then derives a local steady deadline bounded by
  the remaining duration. The local `OperationContext` continues to own
  cancellation and deadline enforcement.
- Methods are `manager.status`, `manager.settings`, `manager.control`,
  `manager.settings.update`, `manager.cancel`, and `manager.shutdown`. The
  protocol codec is manager-specific and does not change the independent MCP
  one-megabyte newline protocol.

### Delivery slices

- P16-A provides the controller, model/envelope codec, current-user identity and
  security descriptor, instance lease, named-pipe client/server, manager host,
  and focused unit/process tests.
- P16-B provides the loopback HTTP listener, bounded request parser/router,
  DPAPI-protected dashboard bearer token, operational/manager/telemetry route
  composition, and port-collision tests. Bespoke styling remains deferred, but
  behavior and bounded transport are not.
- P16-C provides bounded watchdog/restart policy, P15 deployment
  reconciliation, continuity recovery ownership, and idempotent per-user Task
  Scheduler registration, inspection, repair, enable, disable, and removal.
- Focused targets may build and test during development. G16 receives one
  authoritative affected-target rebuild and test invocation only after all
  three slices are present; a passed authoritative gate is not repeated.

## Consequences

The GUI, CLI, session host, and dashboard share one typed manager behavior
without owning manager resources. A process crash releases exclusive ownership
automatically, while settings and authentication material remain separately
durable. The pipe and dashboard transports can be replaced or tested without
changing the application controller.

P16 introduces a dedicated protocol rather than reusing HTTP for privileged
native clients. The dashboard remains a loopback product surface, while the
named pipe is the primary native control plane. This avoids port availability
being a prerequisite for GUI/CLI manager control and provides an exact
current-user identity boundary.

Passing an early slice does not pass G16. Startup, restart, dashboard,
authentication, shutdown, and process-residue evidence remain mandatory.

## Alternatives rejected

- A PID file is not the Windows ownership primitive and can remain stale after
  a crash; the per-user mutex has kernel-owned lifetime.
- A machine-global fixed mutex or pipe name would cause cross-user denial of
  service and would not satisfy per-user ownership.
- Default named-pipe security plus `PIPE_REJECT_REMOTE_CLIENTS` still permits
  other local users and does not satisfy current-user authentication.
- A nonce without SID verification would turn possession of a copied value into
  the only control-plane boundary.
- Loopback HTTP as the sole native control channel would couple lifecycle
  control to dashboard port ownership and browser-oriented request behavior.
- Reusing MCP framing or changing its size limit would conflate two independent
  protocols and weaken already-qualified MCP evidence.
- A detached accept thread, unbounded thread-per-client design, service locator,
  or process-wide mutable singleton would violate explicit lifetime and
  resource requirements.

## Scope and limitations

Alpha qualification is restricted to the owner's current Windows 11 x64
machine. Clean-environment testing, a broad installer matrix, security-only
hardening, and bespoke dashboard/UI polish remain deferred under OWNER-002.
Current-user SID/DACL/nonce authentication, DPAPI storage, loopback binding,
bounded frames and requests, deadlines, cancellation, and deterministic
shutdown remain functional requirements.

UI-011 and CLI manager composition remain open for P20 and P21. P16 supplies
their production backend and typed clients.

## Evidence basis

- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/instructions/architecture/PROCESS_MODEL_AND_IPC.md`
- `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
- `.forge-codex/instructions/architecture/TARGET_ARCHITECTURE.md`
- `.forge-codex/instructions/architecture/SECURITY.md`
- `.forge-codex/instructions/plans/phases.json`
- `.forge-codex/instructions/plans/gates.json`
- `.forge-codex/instructions/plans/test-matrix.json`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `include/ForgeConductor/Contracts/IManagerServices.h`
- `include/ForgeConductor/Domain/ManagerModels.h`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Manager/ManagerNode.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Manager/ManagerRuntime.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Manager/ManagerDashboardClient.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Manager/ManagerInstaller.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/DashboardServer.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/DashboardHTTPRequest.swift`
