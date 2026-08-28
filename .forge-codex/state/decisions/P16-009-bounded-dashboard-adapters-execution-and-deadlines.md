# P16-009: Bounded Dashboard Adapters, Execution, and Deadlines

Status: Accepted

Date: 2026-08-27

## Context

P16-008 closed the transport-neutral dashboard application boundary but left
four concrete ownership gaps: telemetry fan-out without replacing the sole
collector consumer, atomic operational reads and administrative close
semantics, isolation of blocking application calls from IOCP workers, and a
deadline owner that cannot grow stale heap entries. The existing contracts do
not expose a telemetry observer registration, an atomic open/recent/presence
read, raw diagnostic lines, or the released dashboard session-close mutation.
Broadening those established contracts would affect unrelated producers and
repositories.

## Decision

### Telemetry adapter

- `DashboardTelemetrySource` observes the existing collector through an
  explicit synchronous `publish` ingress. It never starts or stops the
  collector and never calls `setConsumer`.
- The manager composition root owns the sole telemetry consumer and forwards
  each accepted snapshot to the dashboard ingress as one leg of its bounded
  fan-out.
- One ingress encodes one immutable compact/full frame pair. The source retains
  one latest observation and the broadcaster gives each of at most 32
  subscribers a capacity-one replaceable frame.
- Construction may seed from `ITelemetryService::latest` without claiming that
  live production is running. Health, latest, and subscription reads enforce
  the active 1 or 2 Hz resource profile and application response bounds.

### Operational adapter

- `IDashboardOperationalDataSource` is the narrow application-owned port for
  one atomic bounded `{open sessions, recent sessions, presence}` snapshot,
  bounded raw diagnostic lines, and the released administrative session-close
  transaction.
- `DashboardOperationalService` composes that port with the existing agent
  catalog/session, audit, doctor, runtime-diagnostic, and tool-catalog
  contracts. It rejects over-bound or invalid source results and never
  truncates them.
- Administrative close accepts any existing session, commits status `closed`,
  stores the already validated summary verbatim, and clears active projections
  atomically. It deliberately does not call the owner-sensitive agent
  completion operation.
- Prune retains the existing agent-service mutation and accepts its complete
  bounded `MaximumSessionQueryRows` result even though the released HTTP
  acknowledgement does not expose that count.

### Handler execution

- `WindowsDashboardHandlerExecutor` owns exactly four persistent workers and a
  FIFO queue of exactly eight pending move-only tasks. Submission never waits;
  saturation is an immediate retryable `LimitExceeded` result.
- The operation type is closed to prepare-exchange and post-delivery work. A
  weak nonblocking completion sink consumes the corresponding move-only typed
  result and is the only path back to the IOCP runtime.
- Caller cancellation, deadline expiry, escaped exceptions, and mismatched
  completion kinds become typed results. Shutdown cancels pending and active
  work and has a five-second fail-fast lifetime boundary.
- Worker closures retain shared implementation ownership. A completion sink
  may synchronously release the outer executor: the current thread handle is
  detached only to avoid self-join, while that worker retains all state through
  exit and the other three workers are joined.

### Deadline execution

- `WindowsDashboardDeadlineScheduler` owns one worker and at most 43 indexed
  entries: 40 connections, two listener generations, and one shutdown drain.
  Entries remain sorted in one pre-reserved vector; replacement erases the
  previous value, so no lazy or stale heap population exists.
- Every owner uses a stable nonzero registration identifier. The scheduler
  assigns and returns a globally monotonic nonzero `armSequence` for every
  accepted arm, including a replacement, so an arm dequeued for delivery can
  never share a token with its successor. Cancellation is exact-token only.
- A sink posts the full immutable deadline as a synthetic IOCP completion. The
  connection or listener state accepts it only when `{registrationId,
  armSequence}` still equals its current arm, clears that token before applying
  the timeout, and otherwise only reaps the stale completion. Sequence
  exhaustion is an integrity failure at the scheduler boundary.
- The sink is weak. Reentrant release from its callback is safe: the deadline
  worker retains the implementation through exit and detaches only its own
  thread handle to avoid self-join.

### Composition lifetime order

The concrete adapters expose close transitions, not permission to destroy an
object under an active member call. Manager teardown therefore follows this
fixed order:

1. Stop listener and handler admission and disable only the dashboard leg of
   telemetry fan-out.
2. Close SSE subscriptions and request cancellation of handler work.
3. Retain both adapters while the bounded handler executor and in-flight
   telemetry publisher count drain.
4. Invoke operational shutdown from a thread holding no operational admission,
   then destroy the connection application and adapters.
5. Only afterward stop or destroy the injected telemetry, diagnostic,
   repository, and clock owners.

This order prevents facade self-drain deadlock and prevents concurrent
destruction from racing a producer or handler call. A dashboard post-delivery
manager-shutdown task does not reenter the operational facade.

## Consequences

The IOCP implementation can remain a nonblocking socket state machine with a
closed application completion path, exact overload behavior, race-proof
deadline arms, and no callback or timer backlog. Existing cross-product
contracts remain stable. The upcoming manager composition must implement the
telemetry fan-out and operational data source and must encode the teardown
order above rather than treating either adapter's `shutdown` as a concurrent
destruction barrier.

This checkpoint does not implement the AcceptEx listener, connection state
machine, two-phase rebind, static shell, UI automation, production manager
composition, or P16-C watchdog/startup recovery. It does not satisfy P16 or
G16 by itself.

## Alternatives rejected

- Replacing the telemetry consumer inside the adapter would silently detach
  another consumer and violate collector ownership.
- Adding observer and exact-read methods to established broad contracts would
  expand unrelated boundaries when one narrow application port is sufficient.
- Running application calls on IOCP workers lets repository or manager latency
  stop all socket progress.
- A deadline heap with lazy invalidation can grow without bound under repeated
  re-arm operations even while live connection count remains fixed.
- Registration identifier alone, or a caller sequence checked only against the
  live index, cannot reject an old timeout or late cancel after the old arm was
  dequeued and the same owner was re-armed.
- Destroying adapters concurrently with member calls cannot be repaired by an
  internal counter acquired through an object whose lifetime may already have
  ended; composition must first close and drain call ownership.

## Evidence basis

- `.forge-codex/state/decisions/P16-006-dashboard-application-and-iocp-ownership-boundaries.md`
- `.forge-codex/state/decisions/P16-007-bounded-dashboard-assets-and-sse-publication.md`
- `.forge-codex/state/decisions/P16-008-closed-dashboard-application-dispatch-and-json-boundary.md`
- `include/ForgeConductor/Application/DashboardOperationalService.h`
- `include/ForgeConductor/Application/DashboardTelemetrySource.h`
- `include/ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h`
- `include/ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/OperationalRoutes.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift`
