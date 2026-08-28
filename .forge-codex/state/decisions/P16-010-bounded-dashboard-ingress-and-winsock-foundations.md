# P16-010: Bounded Dashboard Ingress and Winsock Foundations

Status: Accepted

Date: 2026-08-27

## Context

P16-009 established bounded application execution and deadline scheduling, but
the loopback transport still lacked four ownership primitives needed before an
IOCP listener can be composed: incremental request framing, literal loopback
endpoint validation, balanced Winsock lifetime ownership, and a capacity-one
bridge from the deadline worker to synthetic IOCP completions.

The listener must accept exactly one strict HTTP request per connection without
reparsing every accumulated prefix, must never resolve or bind a non-loopback
host, must close every socket before its Winsock startup reference is released,
and must not turn repeated timer expirations into an unbounded completion
backlog.

## Decision

### Incremental HTTP ingress

- `DashboardHttpParserSession` owns one request and has explicit receiving
  header, receiving body, complete, rejected, and closed states.
- It scans the header delimiter incrementally. The canonical
  `DashboardHttpParser` remains the HTTP-policy authority and is invoked only
  at the header boundary, complete request boundary, stream completion, and
  trailing-byte rejection.
- The session reserves at most the configured maximum request size plus one
  byte. The extra byte lets the canonical parser classify pipelining or body
  overflow without copying an entire maximum-sized request.
- A successful take transfers the request exactly once. Any rejection or
  internal failure immediately releases ingress storage while retaining only
  its bounded typed rejection.

### Literal loopback endpoints

- `DashboardLoopbackEndpoint` accepts only the exact literals `127.0.0.1` and
  `::1` with a nonzero port. It constructs `sockaddr_in` or `sockaddr_in6`
  directly and performs no DNS or host-name resolution.
- Bound-address validation requires the selected loopback address and exact
  configured port. Peer validation requires the same literal loopback family
  and rejects wildcard, alternate `127/8`, IPv4-mapped IPv6, scope, or flow
  metadata.
- The endpoint owns a fixed-size canonical authority representation for later
  configuration and diagnostics without runtime formatting allocation.

### Winsock and socket lifetime

- `DashboardWinsockRuntime` requests and verifies Winsock 2.2 exactly once per
  runtime state and balances it with exactly one `WSACleanup`.
- Socket creation uses TCP stream sockets with
  `WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT` and accepts only IPv4 or
  IPv6 families.
- `UniqueDashboardSocket` is move-only and retains the initialized runtime
  state. It exposes no raw release operation, so `closesocket` necessarily
  precedes the final `WSACleanup`.

### Deadline completion mailbox

- `DashboardDeadlineNotificationMailbox` has a fixed maximum of 43 slots,
  matching 40 connections, two listener generations, and one shutdown drain.
- Accepted registration identifiers must be strictly increasing and are never
  reused. Slot generations independently prevent an already-posted completion
  from consuming a recycled slot.
- Each live owner can have at most one posted synthetic completion. Repeated
  publications replace the immutable latest deadline and return `Coalesced`
  instead of requesting another post.
- Retirement and shutdown discard unpublished values immediately. A slot is
  retained as a bounded tombstone only while its one posted completion remains
  to be reaped.
- The future IOCP adapter must post exactly once for every
  `NotificationRequired` result. If posting fails, it must retire and take the
  returned handle before reporting the failure.

## Consequences

The next slice can build listener generations, four runtime-owned `AcceptEx`
slots, and four IOCP workers on primitives that already enforce strict address,
framing, native-handle, and deadline-backlog bounds. The listener composition
must allocate registration identifiers from one process-lifetime monotonic
sequence and must keep the mailbox alive until all synthetic completions are
reaped.

This checkpoint does not implement `AcceptEx`, the IOCP worker kernel,
connection send/receive state, two-generation rebind, the dashboard shell, UI
automation, production manager composition, or P16-C watchdog and recovery. It
does not satisfy P16 or G16 by itself.

## Alternatives rejected

- Re-running the complete HTTP parser after every fragment makes fragmented
  headers quadratic and obscures the connection's framing state.
- Maintaining a second hard-coded trailing-byte rejection lets parser policy
  and transport behavior drift.
- DNS resolution or wildcard binding expands the dashboard authority beyond
  the configured loopback service.
- Exposing raw socket release permits a socket to outlive its Winsock startup
  reference.
- Posting one IOCP packet per deadline publication creates an unbounded stale
  completion backlog under rapid re-arm or delayed workers.
- Reusing a retired registration identifier lets a delayed scheduler
  publication target its successor even when completion handles also carry a
  slot generation.

## Evidence basis

- `include/ForgeConductor/Dashboard/DashboardHttpParserSession.h`
- `src/Dashboard/DashboardHttpParserSession.cpp`
- `src/Infrastructure/Windows/Detail/DashboardLoopbackEndpoint.h`
- `src/Infrastructure/Windows/Detail/DashboardLoopbackEndpoint.cpp`
- `src/Infrastructure/Windows/Detail/DashboardWinsockRuntime.h`
- `src/Infrastructure/Windows/Detail/DashboardWinsockRuntime.cpp`
- `src/Infrastructure/Windows/Detail/DashboardDeadlineNotificationMailbox.h`
- `src/Infrastructure/Windows/Detail/DashboardDeadlineNotificationMailbox.cpp`
- `tests/Dashboard/DashboardHttpParserSessionTests.cpp`
- `tests/Dashboard/DashboardLoopbackEndpointTests.cpp`
- `tests/Dashboard/DashboardWinsockRuntimeTests.cpp`
- `tests/Dashboard/DashboardDeadlineNotificationMailboxTests.cpp`

