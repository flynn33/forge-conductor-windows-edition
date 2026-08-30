# P16-027: Manager Process Host Transition Lifecycle

Status: Accepted

Date: 2026-08-30

## Context

`ManagerTransitionWorker` must not observe the controller before successful
Manager initialization, and its controller calls must be quiescent before the
dispatcher closes the controller and runtime. Leaving worker start and join as
composition-root conventions would allow a future executable to omit or
misorder either operation.

The process stop owner must also remain nonblocking while a worker-owned
controller operation is active. The worker's exact `shutdown` intentionally
retains and joins its `std::jthread`; calling it from the external stop path
could therefore wait on a non-cooperative controller callback instead of
letting the blocking run frame retain final teardown ownership.

## Decision

- The composition root constructs and injects the controller, dispatcher,
  ingress server, and transition worker. `ManagerProcessHost` owns their
  lifecycle ordering.
- `run` initializes the controller, starts the transition worker, and only then
  exposes ingress. A genuine worker-start failure is propagated unchanged.
- External shutdown closes ingress, requests startup cancellation, invokes the
  worker's nonblocking `beginShutdown`, and begins dispatcher shutdown. The run
  thread performs the worker's exact join before final dispatcher shutdown
  closes the controller and runtime.
- Shutdown before a run performs exact closure synchronously. Every path is
  idempotent, and destruction while a run frame is active remains a terminal
  ownership violation.
- If external shutdown terminally closes the worker during the narrow
  post-initialize, pre-start transition, the resulting `transport_closed` is
  orderly process termination. Other start errors remain failures.
- `IManagerTransitionWorker` makes the two shutdown operations explicit:
  `beginShutdown` closes successor admission and requests cancellation without
  joining; `shutdown` retains exact thread ownership until quiescence.

This decision supersedes only the lifecycle-order clauses in P16-003 and the
composition-root start/join clause in P16-026. Their other decisions remain in
force.

## Consequences

Production composition cannot expose Manager ingress without a running
transition owner, destroy the controller while watchdog or restart work can
execute, or accidentally block the external stop-signal path on the exact
worker join. The run frame is the single final teardown owner whenever it is
active.

This checkpoint does not provide the production Manager composition root,
operational dashboard dependencies, real-process evidence, retained UI
automation, or the authoritative G16 gate.

## Evidence basis

- `src/Hosts/Manager/ManagerProcessHost.h`
- `src/Hosts/Manager/ManagerProcessHost.cpp`
- `src/Hosts/Manager/ManagerTransitionWorker.h`
- `src/Hosts/Manager/ManagerTransitionWorker.cpp`
- `tests/Manager/ManagerProcessHostTests.cpp`
- `tests/Manager/ManagerTransitionWorkerTests.cpp`
- `.forge-codex/state/decisions/P16-003-manager-host-lifecycle-and-composition-boundary.md`
- `.forge-codex/state/decisions/P16-026-capacity-one-manager-transition-watchdog.md`
