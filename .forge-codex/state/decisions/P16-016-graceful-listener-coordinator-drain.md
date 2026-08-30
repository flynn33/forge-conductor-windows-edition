# P16-016: Graceful Listener Coordinator Drain

Status: Accepted

Date: 2026-08-29

## Context

P16-015 established the process-level five-second graceful policy and the
connection state allowed to finish within it. Listener generations and their
coordinator still exposed only hard shutdown: they force-closed accept and
connection ownership, could cancel an already-started final response, and did
not publish an exact process-observable edge after both listener generations
released their completion and deadline routes.

Graceful cutoff must also serialize with rebind publication. Active and
retiring generations may overlap, and a hard or fatal request may arrive while
graceful callbacks are in progress. Per-generation state checks are not
sufficient because hard shutdown can interleave between a check and the next
callback. Collection must not expose a drained edge while any callback from a
previously captured generation snapshot remains pending.

## Decision

### Generation policy

- `IDashboardListenerGeneration` exposes transition-gated
  `beginGracefulShutdown` independently of its existing hard shutdown.
- The concrete graceful path is idempotent. It cancels and removes an exact
  retirement arm, closes new accept admission, cancels exact overload work,
  and schedules the existing after-transition drain pump.
- Graceful listener shutdown does not force-close the listener, arm the
  cancellation-reap watchdog, or shut down existing generation connections.
  The existing hard path remains the exact escalation mechanism.

### Coordinator cutoff and fanout

- The coordinator keeps independent graceful-requested and hard-requested
  latches. A hard-first request suppresses every later graceful callback;
  graceful followed by hard remains a supported escalation.
- Shutdown latching takes the process-shared listener transition gate before
  the coordinator mutex. Rebind holds that same gate across replacement start,
  old-generation retirement, and atomic publication, so shutdown is placed on
  one exact side of publication without holding the coordinator mutex across
  external callbacks.
- One coordinator-owned, mutex-claimed dispatcher serializes every graceful,
  hard, and fatal generation fanout. Concurrent or reentrant requests only
  latch stronger pending work. The dispatcher completes the full active and
  retiring graceful snapshot before selecting queued hard or fatal work.
- Each graceful generation receives a fresh transition guard because each may
  own one distinct after-release drain action. Strong snapshot references keep
  the exact generation identities alive through their callbacks.
- Generation collection is deferred while the dispatcher is active. This
  prevents route unregistration, transient empty state, and process drain
  notification from crossing a pending generation callback.
- Ordinary overload-owner shutdown maps to graceful listener shutdown.
  Overload terminal edges, explicit hard shutdown, fatal IOCP state, rollback,
  and unregister failure retain hard or fatal behavior.

### Exact process drain edge

- Composition may bind one
  `IDashboardListenerGenerationCoordinatorDrainObserver` before shutdown. The
  coordinator retains it weakly; process composition must retain it strongly
  until notification.
- The one-shot edge is eligible only after shutdown admission is closed, no
  preparation, collection, or fanout dispatcher is active, and neither active
  nor retiring generation remains registered.
- The callback runs outside the coordinator mutex and is a latch only. If a
  previously bound observer expires before the edge, the coordinator fails
  fast rather than silently converting exact process shutdown into an
  unbounded wait.

## Consequences

The listener layer can now quiesce new admission while preserving the complete
responses authorized by P16-015. Rebind publication and shutdown cutoff have a
single ordering point, graceful and hard generation callbacks are monotonic,
and process composition receives one exact listener-routing drain edge without
polling.

The handler-executor drain edge, overload settling edge, registry routing
progress edge, concrete five-second `DashboardShutdownDrain`, static frontend
shell, retained UI automation, and P16-C recovery remain required. This
focused checkpoint does not satisfy P16 or G16.

## Alternatives rejected

- Reusing hard generation shutdown for the initial cutover would cancel
  complete-response sends that the owner-approved five-second grace permits.
- Checking the hard latch separately before each graceful callback leaves a
  time-of-check/time-of-use gap in which hard can overtake the remaining
  graceful snapshot.
- Holding the coordinator mutex across generation callbacks would violate the
  reentrant callback contract and can deadlock synchronous drain publication.
- Using one transition guard for both generations cannot represent two
  independent after-release actions.
- Publishing drain as soon as the tracked generation slots become empty can
  fire while a strong snapshotted callback is still pending.

## Evidence basis

- `src/Infrastructure/Windows/Detail/DashboardListenerGeneration.h`
- `src/Infrastructure/Windows/Detail/DashboardListenerGeneration.cpp`
- `src/Infrastructure/Windows/Detail/DashboardListenerGenerationCoordinator.h`
- `src/Infrastructure/Windows/Detail/DashboardListenerGenerationCoordinator.cpp`
- `tests/Dashboard/DashboardListenerGenerationTests.cpp`
- `.forge-codex/state/evidence/P16/dashboard-graceful-listener-coordinator-drain-checkpoint.json`
