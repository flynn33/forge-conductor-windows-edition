# P16-017: Exact Dashboard Shutdown Owner Edges

Status: Accepted

Date: 2026-08-29

## Context

P16-015 defined the process-level five-second graceful policy, and P16-016
made listener generations and their coordinator publish an exact drain edge.
Three other process-owned components still lacked sufficient causal edges for
one shutdown driver to wait without polling:

- the handler executor could report empty work before its four literal worker
  handles had been joined;
- the connection registry did not publish repeatable progress as registered
  targets and retired deadline tombstones released routing ownership; and
- the overload responder set could expose empty slots before detached handoff
  callbacks, source-owner publications, terminal-generation publications, or
  its shared deadline had settled.

Those gaps also made observer lifetime failures ambiguous. A missing process
observer must fail closed before another shutdown wake can observe a false
ready state.

## Decision

### Handler executor

- Process composition may bind one weak
  `IDashboardHandlerExecutorDrainObserver` before shutdown and must retain it
  strongly through the exact edge. Standalone executor use may omit it.
- `beginShutdown` remains nonblocking and closes admission, cancels accepted
  work, and wakes the four persistent workers. A worker-context shutdown with
  process observation bound never joins or detaches itself; the external
  process finalizer retains join authority.
- Exact readiness requires stopped admission, no pending or active task, no
  live post-delivery reservation, and all started worker handles joined by an
  external caller. The one-shot callback is taken under state ownership and
  invoked only after releasing executor locks.
- Thread-local worker identity remains valid through worker-lambda capture
  destruction. If process composition violates its external-finalizer
  lifetime and the implementation is destroyed on its own worker, a
  destruction-only fallback joins every other handle, detaches only the
  already-quiesced current handle, and deliberately does not publish the
  exact joined edge.
- The public `shutdown` call pins implementation ownership across an
  out-of-lock observer callback, so callback-driven destruction cannot race
  method return.

### Registry routing progress

- The registry exposes a monotonic `routingProgressRevision` and a repeatable
  weak `IDashboardConnectionRegistryRoutingProgressObserver`. Managed process
  composition binds once before shutdown and strongly retains the observer
  through final routing release.
- Exact connection removal, auxiliary deadline-target removal, and retired
  deadline tombstone reap each advance the canonical revision.
- A single-owner capacity-one dispatcher publishes the greatest pending
  revision outside both registry and deadline-routing locks. Concurrent and
  reentrant reductions can coalesce intermediate revisions but cannot regress,
  duplicate ownership reduction, or lose the latest wake.
- A shutdown driver compares the callback revision with a fresh snapshot
  before sleeping. This closes the readiness-check/wait race without polling.
- When malformed input both drains a retired tombstone and produces a routing
  failure, the registry records fatal state before advancing and dispatching
  progress. A reentrant driver therefore cannot finalize a nonfatal route in
  the interval between ownership reduction and failure publication.
- The existing process zero-connection edge now covers both graceful and hard
  shutdown. It remains optional for standalone use and fails closed after a
  successful managed bind if its observer expires.

### Overload responder settlement

- Managed composition first binds and retains the existing admission-overload
  source observer, then binds one weak
  `IDashboardOverloadResponderSetDrainObserver`. Standalone responder use may
  omit both process-level requirements.
- Moving work out of a slot increments an explicit settling count before the
  slot becomes empty. Settlement completes only after origin admission,
  paused-token, completion-pending, completion-settled, and applicable
  generation-drain callbacks have returned.
- Ordinary readiness requires its source shutdown publication to have
  completed. Terminal readiness supersedes the ordinary edge and requires the
  owner terminal callback plus every required terminal-pending and terminal
  generation callback to be delivered.
- Exact process readiness additionally requires no active slot, no detached
  settlement, no shutdown transition in progress, and no overload deadline.
  The allocation-free snapshot mirrors every predicate used by the callback.
- A managed source observer that expires at any required publication edge is
  retained as one structural failure, invokes fail-fast once, and permanently
  suppresses readiness.
- Fatal error retention and terminal latching occur under one state
  transition, so ordinary readiness cannot win between the two.
- If the required process observer is absent at the exact edge, readiness
  claiming atomically records `IntegrityFailure`, marks process publication
  failed, and suppresses both internal and snapshot drainage before fail-fast
  runs outside the locks.

## Consequences

The next process-owned `DashboardShutdownDrain` can receive latch-only wakes,
take immutable snapshots, and wait for exact executor, listener, overload,
connection, and routing ownership without polling or observing an empty-but-
still-callback-active state. Observer ownership stays acyclic and a broken
managed lifetime fails closed before teardown can begin.

The concrete five-second process deadline owner, hard escalation driver,
final router/scheduler/IOCP teardown order, static frontend shell, retained UI
automation, and P16-C recovery remain required. This focused checkpoint does
not satisfy P16 or G16.

## Alternatives rejected

- Treating worker-loop exit as executor drainage omits literal thread-handle
  join and can publish from a worker that cannot safely join itself.
- Polling registry snapshots cannot close the race between a readiness check
  and a concurrent tombstone reap without an additional event protocol.
- Publishing every registry revision from its mutating thread under the state
  lock permits reentrant deadlock and unbounded callback contention.
- Treating an empty overload slot as settled releases process ownership before
  the detached handoff returns its token and source callbacks complete.
- Emitting terminal process drainage after only the owner callback can allow
  teardown to overtake required per-generation terminal callbacks.
- Recording missing observers after releasing readiness locks leaves a window
  in which a different component wake can observe false drainage.

## Evidence basis

- `include/ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h`
- `src/Infrastructure/Windows/WindowsDashboardHandlerExecutor.cpp`
- `tests/Dashboard/WindowsDashboardHandlerExecutorTests.cpp`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRegistry.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRegistry.cpp`
- `tests/Dashboard/DashboardConnectionRegistryTests.cpp`
- `src/Infrastructure/Windows/Detail/DashboardOverloadResponderSet.h`
- `src/Infrastructure/Windows/Detail/DashboardOverloadResponderSet.cpp`
- `tests/Dashboard/DashboardOverloadResponderSetTests.cpp`
- `.forge-codex/state/evidence/P16/dashboard-exact-shutdown-owner-edges-checkpoint.json`
