# P16-026: Capacity-One Manager Transition Watchdog

Status: Accepted

Date: 2026-08-30

## Context

The Manager host already exposes a process-owned restart signal so dashboard
and named-pipe requests can acknowledge an operator restart without performing
the restart on an ingress worker. P16 still required one production owner to
consume that signal, reconcile runtime drift on the configured watchdog
cadence, and prove that deferred transitions cannot overlap or grow an
unbounded queue.

The macOS behavior repairs a missing control listener regardless of the
automatic-restart preference. It conditionally recovers a desired-running
operational service when automatic restart is enabled, stops an erroneously
active service when desired state is stopped, and suppresses all watchdog
mutation after shutdown intent is latched.

## Decision

- One `ManagerTransitionWorker` owns one `std::jthread`. Construction is inert;
  the composition root starts it only after `IManagerController::initialize`
  succeeds and joins it before releasing the controller or runtime.
- `ManagerProcessRestartSignal` remains the capacity-one edge for explicit
  restart work. One request may be pending or in flight; additional requests
  coalesce, and terminal close rejects all successors.
- The same worker waits without polling for either an explicit restart or the
  next watchdog deadline. It performs controller calls serially, so explicit
  restart and watchdog repair can never overlap.
- An explicit request invokes `Restart` and completes its signal lease through
  RAII even when context construction or the controller returns a typed error.
  A watchdog correction invokes `Repair`, preserving restart-count semantics
  for operator-requested or binding-changing restarts only.
- Watchdog policy is pure and exact: shutdown performs no work; a missing
  listener is always repaired; undesired active service state is repaired;
  desired-running inactive or failed service state is repaired only when
  automatic restart is enabled.
- The configured watchdog interval is clamped to one through sixty seconds.
  Every observation and mutation receives fresh operation and correlation
  identities, the worker stop token, and a bounded deadline. Production maps a
  watchdog second to one wall-clock second; the timing scale is injectable only
  to keep focused tests deterministic and is itself upper-bounded.
- Shutdown permanently closes the signal, requests cancellation, and retains
  exact `std::jthread` ownership until the worker returns. Concurrent shutdown
  callers share one join owner and cannot publish `Stopped` while work can
  still execute.

## Consequences

There is no transition task queue, timer callback backlog, overlapping runtime
mutation, or detached worker. A restart request racing a watchdog deadline
remains pending and is consumed on the worker's next wait; watchdog work and
that request are still serialized on the same thread.

Typed observation or repair failures are retained by the controller status and
retried only on a later bounded watchdog interval. The worker does not spin or
invent a second retry scheduler. Controller and runtime implementations remain
responsible for honoring the supplied cancellation token and deadline at their
native boundaries.

## Evidence basis

- `src/Hosts/Manager/ManagerProcessRestartSignal.h`
- `src/Hosts/Manager/ManagerProcessRestartSignal.cpp`
- `src/Hosts/Manager/ManagerWatchdogPolicy.h`
- `src/Hosts/Manager/ManagerWatchdogPolicy.cpp`
- `src/Hosts/Manager/ManagerTransitionWorker.h`
- `src/Hosts/Manager/ManagerTransitionWorker.cpp`
- `tests/Manager/ManagerTransitionWorkerTests.cpp`
- `.forge-codex/state/decisions/P16-003-manager-host-lifecycle-and-composition-boundary.md`

