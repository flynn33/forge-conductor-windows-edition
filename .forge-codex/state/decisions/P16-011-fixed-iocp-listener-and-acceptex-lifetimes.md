# P16-011: Fixed IOCP Listener and AcceptEx Lifetimes

Status: Accepted

Date: 2026-08-27

## Context

P16-010 established bounded Winsock, endpoint, ingress, and completion-mailbox
primitives. The dashboard transport still needed a listener-generation owner
that could accept connections without polling, preserve native operation
storage through cancellation, and shut down predictably after partial startup
or provider failure.

The Windows dashboard contract requires one dashboard IOCP, exactly four
persistent socket workers, exactly four outstanding `AcceptEx` operations per
listener generation, a backlog of 40, and a five-second bounded drain. A
dequeued native completion is the sole authority that an overlapped accept no
longer references its slot storage.

## Decision

### IOCP and worker kernel

- `DashboardIoCompletionPort` creates one completion port with concurrency
  four and associates sockets without changing that port-wide limit.
- `DashboardIocpWorkerKernel` owns exactly four persistent workers. Each uses
  a finite 250-millisecond dequeue wait so shutdown and fatal state remain
  observable without an unbounded block.
- Startup has a five-second ready barrier. Shutdown first closes native-call
  admission. Workers continue servicing completions until admitted native
  calls leave their barrier and exactly four reserved control packets are
  posted; a timeout alone cannot let them overtake an admitted call. The whole
  shutdown drain remains bounded to five seconds.
- Fatal dequeue failure is retained as fixed diagnostic state and reported to
  the injected sink at most once. Re-entrant shutdown from that callback is
  permitted, while final joining remains the external owner's responsibility.

### Loopback listener and provider functions

- `DashboardListeningSocket` binds only the configured literal `127.0.0.1` or
  `::1`, verifies the exact bound address, enables exclusive address use, uses
  IPv6-only mode for `::1`, and listens with backlog 40.
- Address conflicts remain typed conflicts. No wildcard address, host-name
  lookup, dual-stack expansion, or address reuse is permitted.
- `DashboardWinsockExtensions` discovers `AcceptEx` and
  `GetAcceptExSockaddrs` from the exact listener socket. The function pointers
  cannot be supplied by a different provider generation.
- Each accept uses zero receive bytes and fixed local and remote address
  regions sized for the maximum socket address plus the required 16-byte
  padding.

### Accept slots and generation owner

- Each `DashboardAcceptSlot` is heap-stable and non-movable. It owns its
  accepted socket, fixed address buffer, and `OVERLAPPED` for the entire native
  operation lifetime.
- Slot state is published as issued before the native `AcceptEx` call. Issue,
  cancellation, and reap are serialized so an immediate completion cannot
  race an unpublished operation.
- Cancellation requests never release slot storage. Both a successful
  `CancelIoEx` request and `ERROR_NOT_FOUND` retain the issued operation until
  its exact completion packet is reaped.
- Successful reaping applies `SO_UPDATE_ACCEPT_CONTEXT`, extracts owned local
  and remote addresses, validates the exact local bind and loopback peer, and
  transfers a move-only accepted connection with a one-shot reissue token.
- `DashboardAcceptSlotSet` owns exactly four slots for one immutable listener
  generation. It associates the listener before issuing any slot and preserves
  partial-start state until every issued operation drains.
- Completion routing requires the generation key, one of the four exact
  `OVERLAPPED` addresses, and the zero-byte AcceptEx contract. An integrity
  violation belonging to an issued slot is consumed through that slot before
  admission closes, so the sole dequeued operation cannot be stranded.
- A successful slot is not reissued before the caller applies the shared
  connection-admission decision. Returning its exact move-only token reissues
  that slot only while generation admission remains open. Stale, duplicate,
  cross-generation, or cross-set tokens cannot mutate slot state.
- Closing admission leaves a token-outstanding slot retained but performs no
  native cancellation for it. Returning that token after close drains the slot
  without issuing, so the generation owner cannot be destroyed while a token
  still identifies it.
- Withholding the four tokens while fixed overload responses are in flight
  bounds overload send ownership to four and ensures another socket is not
  accepted until one responder finishes. This preserves the required 503
  response without a user-space connection queue.
- Only a retryable per-client transport close automatically re-primes a failed
  slot. Listener-wide, integrity, provider, or explicit token-return issue
  failure closes admission and requests cancellation for the other issued
  slots.
- Shutdown snapshots and the public close result contain only fixed,
  allocation-free lifecycle classifications. The first complete typed error
  remains owned for a separate explicit copy path that is not marked
  `noexcept`.

## Consequences

The next transport slice can attach bounded receive/send connection owners and
two-generation rebind logic without changing listener or overlapped-operation
ownership. A retiring generation must keep its slot set, listener, provider
functions, runtime, and IOCP kernel dependencies alive until all four slots are
drained.

This checkpoint does not implement connection admission counts, HTTP
receive/send state, SSE ownership, two-generation rebind, graceful connection
drain, the dashboard shell, UI automation, production manager composition, or
P16-C watchdog and recovery. It does not satisfy P16 or G16 by itself.

## Alternatives rejected

- Creating one thread or one event per accept would violate the fixed IOCP
  ownership and resource budget.
- Discovering extension pointers from a disposable probe socket could mix
  provider generations.
- Freeing an accept buffer after requesting cancellation would permit the
  kernel to complete into released storage.
- Reusing an accept slot after every retryable-looking native failure would
  hide listener-wide failure and create an unbounded retry loop.
- Returning an integrity error without consuming its already-dequeued exact
  operation would make safe destruction impossible.
- Copying string-owning errors while holding the shutdown path's `noexcept`
  contract would turn allocation failure into process termination.

## Evidence basis

- `src/Infrastructure/Windows/Detail/DashboardIoCompletionPort.h`
- `src/Infrastructure/Windows/Detail/DashboardIoCompletionPort.cpp`
- `src/Infrastructure/Windows/Detail/DashboardIocpWorkerKernel.h`
- `src/Infrastructure/Windows/Detail/DashboardIocpWorkerKernel.cpp`
- `src/Infrastructure/Windows/Detail/DashboardListeningSocket.h`
- `src/Infrastructure/Windows/Detail/DashboardListeningSocket.cpp`
- `src/Infrastructure/Windows/Detail/DashboardWinsockExtensions.h`
- `src/Infrastructure/Windows/Detail/DashboardWinsockExtensions.cpp`
- `src/Infrastructure/Windows/Detail/DashboardAcceptSlot.h`
- `src/Infrastructure/Windows/Detail/DashboardAcceptSlot.cpp`
- `src/Infrastructure/Windows/Detail/DashboardAcceptSlotSet.h`
- `src/Infrastructure/Windows/Detail/DashboardAcceptSlotSet.cpp`
- `tests/Dashboard/DashboardIoCompletionPortTests.cpp`
- `tests/Dashboard/DashboardIocpWorkerKernelTests.cpp`
- `tests/Dashboard/DashboardListeningSocketTests.cpp`
- `tests/Dashboard/DashboardWinsockExtensionsTests.cpp`
- `tests/Dashboard/DashboardAcceptSlotTests.cpp`
- `tests/Dashboard/DashboardAcceptSlotSetTests.cpp`
