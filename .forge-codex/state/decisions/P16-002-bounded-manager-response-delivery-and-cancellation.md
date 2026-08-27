# P16-002: Bounded Manager Response Delivery and Cancellation

Status: Accepted

Date: 2026-08-27

## Context

The first P16 named-pipe implementation completed an overlapped response write
and then disconnected the pipe. Windows may discard unread buffered pipe data
when `DisconnectNamedPipe` runs, so write completion alone could not prove that
the client had received a normal response or remote-shutdown acknowledgement.
`FlushFileBuffers` would wait for client consumption but has no operation
deadline and could make manager shutdown unbounded.

The first client also performed its best-effort `manager.cancel` exchange on the
original caller thread after a read or write deadline. That cleanup could add a
fresh two-second delay after the public operation deadline. Separately, joining
fixed server workers without a bound could hang `run()` forever if an injected
controller violated its cancellation contract.

## Decision

- A successful response exchange ends with a fixed transport receipt frame:
  four little-endian length bytes followed by ASCII `FCR1`. The client writes
  this receipt immediately after reading a complete response frame and before
  decoding or returning it.
- The server writes the response and waits for the exact receipt under the
  earlier of the request deadline and `shutdownDrainTimeout`. It disconnects
  only after receipt or bounded failure. Remote shutdown signals ingress only
  after this delivery attempt.
- The receipt is transport framing, not a seventh manager protocol method. It
  carries no identifier, credential, or product data and has one exact value.
- Best-effort cancellation is owned by one client worker with a queue bounded
  by `maximumConcurrentClientRequests`. The public call enqueues cancellation
  and returns its original typed cancellation/deadline error without waiting
  for a second exchange. Client shutdown closes admission, clears the queue,
  signals native I/O, and joins the worker.
- Server workers retain shared ownership of their complete implementation.
  Shutdown cancels native I/O and dispatcher work, then waits only
  `shutdownDrainTimeout`. A worker still executing a non-cooperative controller
  callback is detached from the join set while retaining all referenced state;
  it releases that state when the callback returns.
- Client and server creation enforce the same hard limits: request lifetime at
  most five minutes, connect timeout at most two seconds, shutdown drain at
  most five seconds, no more than sixteen client requests, and the existing
  two-mebibyte frame ceiling.

## Consequences

Normal responses and shutdown acknowledgements have an explicit bounded
delivery boundary without an unbounded Windows pipe flush. Caller deadlines no
longer include cancellation cleanup, while the manager still receives an
idempotent cancellation request through a reserved control worker.

A controller callback that literally never returns keeps one of the fixed
workers, its connected pipe, dispatcher/controller, and server implementation
alive until process exit. This exceptional path is bounded and memory-safe for
the manager run loop, but it cannot promise in-process pipe-name release. The
production controller is required to honor cancellation; the retained-state
behavior exists to prevent destruction under an ABI callback that violates
that contract.

## Evidence

- `.forge-codex/state/commands/20260827T185053660Z-7c618d6e.json`
- `.forge-codex/state/commands/20260827T185117173Z-a5b8746c.json`
- `.forge-codex/state/commands/20260827T185336679Z-ba8cf6e7.json`
- `.forge-codex/state/commands/20260827T185421915Z-c1f07fdf.json`
- `tests/Manager/ManagerPipeInfrastructureTests.cpp`
- `tests/Manager/ManagerRequestDispatcherTests.cpp`
- `tests/Manager/WindowsManagerNamedPipeRoundTripTests.cpp`
