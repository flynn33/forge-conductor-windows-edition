# P16-025: Terminal Manager Pipe Worker Ownership

Status: Accepted

Date: 2026-08-30

## Context

The fixed Manager named-pipe server cancels native I/O and dispatcher work,
then gives every worker `shutdownDrainTimeout` to leave its admitted callback.
P16-002 allowed a worker that crossed that deadline to detach while retaining
the server implementation. That protected the callback's immediate memory but
allowed `run()` to return successfully while the worker could still execute.

The production composition root must release the per-user instance lease only
after Manager ingress and every dependency have stopped. A detached worker can
still reach the dispatcher, controller, pipe handle, clock, identity, and
authentication state after that release. The old successful-return boundary
therefore could permit a successor Manager to acquire process ownership while
code from the previous owner was still running.

## Decision

- No Manager named-pipe worker is detached.
- Shutdown still closes dispatcher admission, signals native I/O, and waits the
  configured bounded `shutdownDrainTimeout` for cooperative worker quiescence.
- Crossing that deadline is an integrity failure and a process-terminal
  cancellation-contract violation. The production fail-fast boundary invokes
  `std::terminate` before the `run()` frame, process composition, or instance
  lease can unwind.
- The production server constructs its own fail-fast owner. A focused injected
  verification seam may return so tests can observe the boundary without
  terminating their runner. If it returns, the server synchronously retains
  the complete implementation and all worker handles behind exact joins. It
  can return only after every worker has left its callback, and that return is
  the retained `integrity_failure`, never success.
- The worker count remains fixed and bounded. Native shutdown continues to
  cancel idle pipe connection waits, so cooperative shutdown is unchanged and
  remains bounded.
- This decision supersedes only the detached-worker rule and retained-state
  successful run-loop consequence in P16-002. Its delivery receipt, client
  cancellation, and transport-limit decisions remain accepted.

## Consequences

A normal return from `WindowsManagerNamedPipeServer::run` now proves that every
fixed worker has passed `workerStopped` and every literal `std::jthread` handle
has joined. The future composition root may therefore release the instance
lease after the host returns without leaving old named-pipe code runnable.

A controller that ignores cancellation past the configured bound terminates
the Manager instead of degrading into an unowned retained worker. This trades
availability for exact process ownership, matching the Task Scheduler COM
worker policy in P16-023. The per-user startup owner or external watchdog may
restart the terminated Manager.

The returning test seam is not a production recovery mode. It exists only to
prove that fail-fast is invoked once, the run frame cannot return while the
callback remains active, and the retained failure follows exact worker
quiescence.

## Evidence basis

- `include/ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeServer.h`
- `src/Infrastructure/Windows/WindowsManagerNamedPipeServer.cpp`
- `tests/Manager/ManagerPipeInfrastructureTests.cpp`
- `.forge-codex/state/decisions/P16-002-bounded-manager-response-delivery-and-cancellation.md`
- `.forge-codex/state/decisions/P16-003-manager-host-lifecycle-and-composition-boundary.md`
- `.forge-codex/state/decisions/P16-023-bounded-task-scheduler-adapter-and-terminal-com-ownership.md`
