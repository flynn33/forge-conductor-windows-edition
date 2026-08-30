# P16-018: Process-Owned Dashboard Shutdown Drain

Status: Accepted

Date: 2026-08-29

## Context

P16-015 established the five-second graceful policy, P16-016 made listener
generations publish exact process drainage, and P16-017 added exact executor,
registry-routing, and overload-settlement edges. The dashboard still lacked
one process owner that could bind those edges before admission, arm an
independent deadline, resolve natural-versus-hard completion races, and retain
the Windows owners until their exact teardown order completed.

Deadline infrastructure also needed managed liveness edges. A scheduler worker
or IOCP bridge can fail independently of connection state, and a callback from
the bridge must never re-enter shutdown while the registry holds its deadline-
routing mutex. Finalization must distinguish live routes, exact posted-retired
tombstones, and zero routing ownership without stopping the IOCP workers before
the last tombstone is reaped.

## Decision

### Process owner and installation

- `DashboardShutdownDrain` is one shared, final process state machine. Its
  abstract host port keeps policy independent of concrete Windows owners; the
  production `DashboardShutdownDrainHost` borrows unique process owners and
  holds only weak references into the shared routing graph.
- `create` starts one persistent driver and proves it ready before returning.
  `install` then binds the bridge and scheduler failure edges, executor,
  listener, registry connection and routing-progress edges, and overload edge
  before registering the drain as an auxiliary deadline target.
- Observer callbacks only latch immutable state and wake the driver. The
  driver alone owns hard fanout, teardown, terminal publication, and fail-fast.
  A partial installation failure waits for that driver-owned terminal work;
  the installer never performs a concurrent teardown fanout.

### Five-second policy and winner selection

- Graceful shutdown first schedules an exact monotonic
  `ShutdownDrain` deadline at current time plus five seconds. Only after the
  arm is retained does it close runtime and handler admission, request
  graceful listener and registry shutdown, stop overload admission, and start
  the executor finalizer.
- A transient executor-finalizer thread owns the potentially blocking four-
  worker join. The persistent driver remains responsive to the independent
  deadline and can issue hard listener, overload, registry, and router fanout
  while that join is blocked.
- Natural completion and hard expiry have one atomic winner. Natural readiness
  moves the current arm into a finalization-cancellation token before teardown;
  a later completion is stale and cannot overtake it. A completion delivered
  while scheduling is still returning is retained and makes hard escalation
  win.
- Exact cancellation is advisory. If the scheduler already published the arm,
  `cancel` returns false; unregistering the drain retires the exact bridge
  owner and leaves its posted operation as a tombstone until IOCP reaps it.

### Managed health and routing edges

- The scheduler and deadline bridge expose optional one-shot weak failure
  observers. Managed composition binds them before work and must retain the
  drain strongly through shutdown; losing a bound observer at the failure edge
  is process-fatal.
- The bridge retains the first fatal transition immediately. A move-only
  failure-notification deferral prevents managed callback delivery from
  overlapping each registry routing transaction, while final release delivers
  the edge only after registry and routing locks are gone.
- `DashboardConnectionRegistry::finalizeDeadlineRouting` is a typed boundary.
  It returns `Pending` for registered routes or exact posted-retired
  tombstones, fails on fatal or malformed ownership, and shuts down the bridge
  only after routing ownership reaches zero.
- Registry routing-progress revisions wake the drain after tombstone reap. The
  drain compares callback and snapshot revisions around every wait, closing
  the finalization lost-wake window without polling.

### Exact terminal teardown

The natural and returning fail-fast verification paths preserve one ordered
release protocol:

1. join the executor finalizer;
2. cancel the natural arm if it remains cancellable;
3. unregister the overload deadline route and fixed completion route;
4. unregister the drain's own auxiliary deadline route;
5. close the completion router and verify all application routes are zero;
6. shut down and join the deadline scheduler;
7. finalize registry and bridge deadline routing while IOCP remains live;
8. shut down and join the four-worker IOCP kernel; and
9. recheck dependency and fatal state before publishing `Drained`.

Production fail-fast terminates. A returning test seam still joins executor
work and proves the same bounded platform teardown before it exposes the
retained terminal error.

## Consequences

Dashboard shutdown now has a bounded owner that can preserve authorized
responses for five seconds, escalate independently of a blocked executor join,
and release scheduler, bridge, registry, router, and IOCP ownership without
polling or abandoning a posted deadline packet. The hybrid real-component
integration test proves the cancel-false tombstone path through the scheduler,
bridge, IOCP port, four-worker kernel, router, registry progress edge, and
finalization retry.

The production Manager composition root has not yet constructed and retained
this graph, so installed-process behavior is not yet proved. P16-C watchdog,
startup, and recovery work, live loopback/browser validation, and retained
native UI automation also remain pending. This focused checkpoint does not
complete P16 or G16, and the authoritative G16 invocation has not been run.

## Alternatives rejected

- Blocking the shutdown driver in executor join would prevent the independent
  five-second deadline from issuing hard fanout.
- Letting natural teardown cancel without first claiming the arm would allow a
  concurrent deadline callback to overtake route release.
- Treating `cancel(false)` as lost work would destroy bridge operation storage
  while an exact IOCP packet still referenced it.
- Stopping IOCP before registry finalization would strand posted-retired
  tombstones and make bridge destruction unsafe.
- Polling routing snapshots would retain the finalization readiness/wait race
  already closed by monotonic routing-progress revisions.
- Invoking bridge failure observers inside registry routing ownership would
  permit callback re-entry under the routing mutex.
- Running partial-install teardown from both the installer and driver would
  create two owners for hard fanout and borrowed dependency release.

## Evidence basis

- `include/ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h`
- `src/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.cpp`
- `src/Infrastructure/Windows/Detail/DashboardDeadlineIocpBridge.h`
- `src/Infrastructure/Windows/Detail/DashboardDeadlineIocpBridge.cpp`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRegistry.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRegistry.cpp`
- `src/Infrastructure/Windows/Detail/DashboardShutdownDrain.h`
- `src/Infrastructure/Windows/Detail/DashboardShutdownDrain.cpp`
- `tests/Dashboard/WindowsDashboardDeadlineSchedulerTests.cpp`
- `tests/Dashboard/DashboardDeadlineIocpBridgeTests.cpp`
- `tests/Dashboard/DashboardConnectionRegistryTests.cpp`
- `tests/Dashboard/DashboardShutdownDrainTests.cpp`
- `tests/Dashboard/DashboardShutdownDrainTombstoneIntegrationTests.cpp`
- `.forge-codex/state/evidence/P16/dashboard-process-shutdown-drain-checkpoint.json`
