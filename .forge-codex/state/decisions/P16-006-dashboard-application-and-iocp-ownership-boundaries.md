# P16-006: Dashboard Application and IOCP Ownership Boundaries

Status: Accepted

Date: 2026-08-27

## Context

P16-004 and P16-005 fix the loopback protocol, authentication, parser,
planning, response, connection, and stream bounds. The remaining P16-B work
must connect those pure components to blocking application services and native
Winsock without letting socket workers execute business operations, creating a
thread per connection, duplicating telemetry production, or invalidating an
in-flight response during listener replacement.

The existing manager named-pipe implementation owns pipe-specific completion
keys, one worker, and pipe-operation registrations. It is not a general socket
runtime. The dashboard also needs an explicit boundary for the shutdown route:
the acknowledgement must finish on the wire before manager shutdown begins.

## Decision

### Layer and application boundary

- Transport-neutral dashboard exchange models and application-facing
  interfaces belong to `ForgeConductor.Dashboard.Protocol`. The concrete
  request handler belongs to `ForgeConductor.Application`; the Winsock server
  belongs to a Windows infrastructure target. Protocol never depends on
  Application or Windows infrastructure.
- The connection application accepts one owned, already parsed request plus
  operational-service state and a bounded `OperationContext`. It returns a
  closed move-only exchange: either fully encoded complete-response bytes and
  an enum-only post-delivery action, or a fully encoded SSE bootstrap with a
  bounded subscription. The transport does not call a controller or reinterpret
  routes, response policy, or JSON.
- Manager routes depend only on injected `IManagerClient`. No dashboard object
  references `IManagerController`, constructs a second manager composition
  root, or calls manager shutdown while preparing the HTTP response.
- `RequestManagerShutdown` is the only shutdown post-delivery action. A
  connection schedules it exactly once on the handler executor after the final
  acknowledgement send completes successfully, using a fresh bounded context.
  Send failure suppresses the action.
- Each listener generation owns an immutable request handler containing that
  generation's endpoint and bearer policy. A retiring generation therefore
  retains the exact policy under which its admitted requests began.

### Handler execution and response bounds

- IO completion workers perform only socket state transitions. Potentially
  blocking manager, repository, telemetry, asset, and operational calls run on
  one separate four-worker handler executor with a queue capacity of eight.
  Saturation rejects immediately; no connection owns a worker or thread.
- Handler composition reserves the complete 16,384-byte response-header budget
  before requesting JSON or asset bodies. Its maximum safe body input is
  2,080,768 bytes. A result that cannot fit the aggregate 2,097,152-byte wire
  ceiling becomes a small typed error response and is never truncated.
- Static assets are loaded only through an injected bounded asset store using
  the already canonical `DashboardStaticResourcePath`. Telemetry is read and
  subscribed only through one application-owned source; handlers do not start
  collectors or install competing consumers.
- An SSE publication owns one immutable compact/full encoded frame pair.
  Every subscription has one replaceable pending pair and signals its stable
  weak ready sink only on an empty-to-nonempty transition. Each connection
  chooses the full representation for every tenth frame actually delivered.

### Winsock and admission ownership

- The dashboard uses overlapped Winsock2 `AcceptEx` sockets associated with
  one process-owned IO completion port and exactly four I/O workers across
  active and retiring listener generations. It does not reuse the pipe-specific
  completion-port helper.
- One runtime-owned admission controller is shared by all generations. An
  accepted connection first reserves one of eight short/unclassified slots.
  SSE classification atomically converts that lease to one of 32 SSE slots.
  Counts never exceed eight short, 32 SSE, or 40 total; failed admission or
  conversion receives a fixed 503 and no user-space wait queue is created.
- Listener sockets bind directly to only the configured literal loopback
  address, use `SO_EXCLUSIVEADDRUSE`, and never use DNS, fallback addresses, or
  `SO_REUSEADDR`. IPv6 loopback listeners are IPv6-only. The bound local and
  connected peer endpoints are verified before request processing.
- A connection owns at most one outstanding receive or send and remains in one
  explicit state. All `OVERLAPPED` storage is registered before issue and
  retained until its completion is reaped, including after cancellation and
  socket close. Native operation storage is never detached during shutdown.
- Four persistent accept slots feed a bounded request-buffer pool. Header,
  body, response, and parser ceilings remain those fixed by P16-004. The OS
  listen backlog is explicitly bounded to 40 and is not treated as a product
  work queue.

### Deadline, rebind, and shutdown ownership

- One deadline owner uses a bounded indexed deadline structure and posts
  synthetic completions to the shared IOCP. There is at most one live deadline
  entry per connection or listener generation; expired lazy-heap entries may
  not accumulate.
- Header ingress ends two seconds after admission. A prepared short request
  receives a five-second application context and has a fifteen-second absolute
  socket ceiling. An SSE connection expires after one hour. Listener retirement
  and process shutdown have a five-second hard drain ceiling.
- Rebind is two phase. The runtime prepares, binds, listens, and validates a
  new generation before publishing it. Failure leaves the active generation
  unchanged. On success the new generation begins admission before the old
  generation stops accepting, and the old generation's connections drain for
  at most five seconds under the shared global admission controller.
- At most one retiring generation is retained. Another rebind while that slot
  is occupied fails with a retryable typed conflict rather than growing a
  generation list.
- Shutdown closes SSE connections immediately, permits only already-prepared
  short final sends inside the drain interval, force-closes at the deadline,
  reaps all native completions, then joins the fixed workers and ends Winsock.
  Failure to reap native operations by the hard lifetime boundary fails fast;
  it never permits detached access to destroyed owners.

## Consequences

Socket behavior, application dispatch, and post-delivery lifecycle can be
tested independently. Blocking service calls cannot stall IO completion, and
listener replacement cannot multiply worker, connection, stream, or queue
budgets. The move-only exchange makes response encoding and shutdown ordering
explicit at the boundary rather than relying on callbacks captured by socket
objects.

The runtime requires more native ownership machinery than a synchronous
thread-per-connection server: listener generations, admission leases, socket
operations, deadline entries, and executor tasks all need explicit owners and
shutdown tests. That work remains incomplete until the concrete server,
handler, telemetry broadcaster, asset store, manager runtime composition, and
focused socket/UI tests pass. This decision alone does not satisfy P16 or G16.

## Alternatives rejected

- Running handlers on IOCP workers allows a named-pipe or repository call to
  stop socket progress for every connection.
- Reusing the named-pipe completion helper couples unrelated completion keys,
  ownership registries, and worker-count assumptions.
- Per-listener admission allows active and retiring generations to double all
  documented capacity limits during rebind.
- A queued callback for every telemetry update creates an unbounded slow-client
  backlog even when frame storage itself is bounded.
- An arbitrary post-send callback lets transport objects capture application
  owners and obscures exactly-once shutdown ordering.
- Rebinding in place can destroy the settings request whose response reports
  the new endpoint and can leave no working listener when the new bind fails.

## Evidence basis

- `.forge-codex/state/decisions/P16-004-bounded-loopback-dashboard-protocol-and-authentication.md`
- `.forge-codex/state/decisions/P16-005-bounded-dashboard-request-planning-and-response-encoding.md`
- `.forge-codex/state/decisions/P16-003-manager-host-lifecycle-and-composition-boundary.md`
- `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
- `.forge-codex/instructions/architecture/PROCESS_MODEL_AND_IPC.md`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/ManagerRoutes.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/OperationalRoutes.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift`
