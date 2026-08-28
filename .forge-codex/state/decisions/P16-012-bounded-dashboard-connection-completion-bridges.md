# P16-012: Bounded Dashboard Connection Completion Bridges

Status: Accepted

Date: 2026-08-27

## Context

P16-011 established the fixed four-worker IOCP listener and four heap-stable
`AcceptEx` slots for each listener generation. The next dashboard transport
slice needs to retain accepted sockets through overlapped receive, send, and
cancellation; deliver handler and SSE-ready events into the same IOCP without
creating per-connection threads or event handles; deliver deadline scheduler
notifications without polling; and guarantee capacity for a post-delivery
operation before a successful response becomes externally visible.

The accepted-socket owner and both synthetic-completion bridges must keep every
native address stable until the kernel's exact queued packet is consumed.
Shutdown therefore cannot treat cancellation or logical retirement as proof
that an outstanding packet no longer references its owner.

## Decision

### Post-delivery executor reservations

- The handler executor's eight pending positions are shared by queued work and
  live move-only post-delivery reservations.
- A connection reserves capacity before sending the first byte of a response
  whose successful delivery requires a follow-up operation. If no reservation
  is available, the connection can return a fixed overload response without
  exposing an acknowledgement that cannot be honored.
- A reservation converts atomically into exactly one FIFO `PostDelivery` task.
  Null operations, the wrong completion kind, expired contexts, cancelled
  contexts, moved-from tokens, and shutdown preserve or reject ownership with
  typed results. Destruction and explicit release return unused capacity once.
- The executor implementation remains shared by live tokens, so a token can
  safely outlive the public executor facade. Shutdown invalidates submission
  but does not invalidate the token's capacity-release obligation.

### Deadline-to-IOCP bridge

- One process-owned bridge contains exactly 43 independently heap-stable
  `OVERLAPPED` slots, matching the scheduler and mailbox hard maximum.
- Each slot retains the mailbox's exact index, generation, and registration
  identity from publication through its sole IOCP reap.
- Only an empty-to-pending transition posts a packet. Later arms for the same
  owner coalesce in the capacity-one mailbox and replace the immutable value.
- Retiring or shutting down a posted owner creates a tombstone; the bridge
  cannot release that slot until the already-posted packet is reaped.
- A matching malformed packet consumes its exact obligation before reporting
  an integrity failure. A foreign operation address cannot consume or vacate
  any slot and closes publication as a retained routing-integrity failure.

### Accepted connection socket

- `DashboardConnectionSocket` consumes and retains the complete
  `DashboardAcceptedConnection`, preserving its socket and endpoint evidence
  for the full connection lifetime.
- The owner has one fixed 16-KiB receive buffer, one stable `OVERLAPPED`, and
  one stable `WSABUF`. At most one receive or send is issued at a time.
- Synchronous overlapped success remains issued because IOCP still owns the
  completion obligation. Borrowed send bytes remain owned by the caller until
  the exact send completion is reaped.
- Receive EOF, zero-byte send, partial send, issue failure, cancellation races,
  shutdown, completion failure, wrong key, wrong pointer, and impossible byte
  counts produce typed bounded outcomes. An exact malformed owned completion
  is consumed before failure; a foreign pointer cannot mutate state.
- `CancelIoEx` success and `ERROR_NOT_FOUND` retain operation storage until
  the completion wins and is reaped. Destruction with an issued operation is a
  fail-fast ownership violation.

### Per-connection synthetic event bridge

- Each admitted connection owns one stable synthetic `OVERLAPPED` and one
  capacity-one bridge carrying at most one move-only handler completion plus a
  coalesced SSE-ready bit.
- Handler and SSE producers post only on the empty-to-pending transition. A
  single reap transfers both currently latched payloads to the IOCP owner.
- Duplicate handler completion, post failure, or malformed exact completion
  becomes retained fatal state. Shutdown converts a posted operation to a
  payload-free tombstone that must still be reaped.
- Move-only handler payload destruction and fatal callbacks occur after the
  bridge mutex is released. The bridge self-retains while invoking a re-entrant
  fatal sink so the callback cannot destroy live operation storage.

## Consequences

The connection-state implementation can now compose exactly one native socket
operation, one synthetic handler/SSE operation, and one coalesced deadline
notification per admitted connection. It can reserve a post-delivery slot
before response delivery, arm bounded header/application/socket lifetimes, and
close only after every native and synthetic obligation is drained.

The next slice must implement the connection lifecycle, central registry,
request parsing and response sequencing, SSE bootstrap and capacity-one frame
delivery, two-generation listener rebind and drain, overload responders, and
server composition. This checkpoint does not satisfy P16 or G16 by itself.

## Alternatives rejected

- Submitting post-delivery work after response transmission would retain a
  capacity race between acknowledgement and side-effect admission.
- Allocating one wait thread, event handle, or unbounded callback queue per
  connection would violate the fixed worker and bounded-resource contracts.
- Reusing an `OVERLAPPED` after logical cancellation or retirement but before
  exact reap would permit the kernel to reference repurposed storage.
- Posting every deadline or SSE-ready signal would allow completion backlog to
  grow independently of the number of connections.
- Destroying move-only completion payloads or invoking fatal callbacks under a
  bridge lock would permit re-entrant deadlock and lifetime inversion.

## Evidence basis

- `include/ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h`
- `src/Infrastructure/Windows/WindowsDashboardHandlerExecutor.cpp`
- `src/Infrastructure/Windows/Detail/DashboardDeadlineIocpBridge.h`
- `src/Infrastructure/Windows/Detail/DashboardDeadlineIocpBridge.cpp`
- `src/Infrastructure/Windows/Detail/DashboardConnectionSocket.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionSocket.cpp`
- `src/Infrastructure/Windows/Detail/DashboardConnectionEventBridge.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionEventBridge.cpp`
- `tests/Dashboard/WindowsDashboardHandlerExecutorTests.cpp`
- `tests/Dashboard/DashboardDeadlineIocpBridgeTests.cpp`
- `tests/Dashboard/DashboardConnectionSocketTests.cpp`
- `tests/Dashboard/DashboardConnectionEventBridgeTests.cpp`
