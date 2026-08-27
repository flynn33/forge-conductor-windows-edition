# P16-007: Bounded Dashboard Assets and SSE Publication

Status: Accepted

Date: 2026-08-27

## Context

P16-006 assigns static content and telemetry fan-out to application-owned,
transport-neutral components, but it does not fix their construction bounds or
the ready-notification behavior when a transport sink is attached after a frame
is already pending. Those details must be deterministic before the concrete
application handler and IOCP transport are composed.

The complete-response wire limit is 2,097,152 bytes and reserves 16,384 bytes
for headers. Static content therefore cannot use the full wire limit as a body.
Telemetry must retain the Windows profile's capacity-one latest-value behavior
without losing the first frame or accumulating callbacks for a slow client.

## Decision

### Static content

- The application composition root constructs one immutable static-asset store
  before publishing a listener generation. Lookups perform no filesystem or
  network I/O and return shared immutable handles that remain valid after the
  store is destroyed.
- A store admits at most 128 canonical assets, at most 8 MiB in aggregate, and
  at most 2,080,768 bytes per asset. Empty, duplicate, oversized, or
  noncanonical definitions fail construction without publishing a partial
  store.
- The four shell route identifiers are a closed set: root, index, control, and
  manager. Production composition registers every identifier. A shell mapping
  must reference one registered canonical HTML asset; multiple identifiers may
  share the same immutable asset.
- MIME types come only from the validated canonical static-resource path. All
  assets use `Cache-Control: no-store` in the alpha profile.

### Response composition

- Complete responses reserve the entire 16,384-byte header ceiling before
  accepting a body, fixing the maximum body at 2,080,768 bytes. Bodies are
  never truncated.
- Error codes are limited to 128 UTF-8 bytes and messages to 4 KiB. Invalid
  internal metadata, unsupported status metadata, or encoding overflow becomes
  one fixed small 500 response without reflecting the rejected values.
- A HEAD rejection encodes the length of the corresponding JSON error body but
  owns no body and no post-delivery action.

### Telemetry publication

- One producer event is encoded once into a shared immutable compact/full pair.
  The public pair factory deep-copies caller buffers before publication, so a
  retained mutable alias cannot alter bytes or violate the validated size after
  construction.
  The compact representation omits Forge and power detail, retains at most the
  first eight process rows, and retains the newest twenty history points. The
  full representation remains the existing complete telemetry event.
- One broadcaster admits at most 32 live subscriptions. Every subscription is
  seeded with a non-null initial pair, retains exactly one replaceable pending
  pair, records one validated finite delivery rate in the closed range 1 to 2
  Hz, and closes idempotently.
- The prepared-exchange boundary revalidates that exposed delivery rate after
  type erasure and closes implementations reporting NaN, infinity, or a value
  outside the supported range.
- Publication signals a ready sink only when the mailbox changes from empty to
  nonempty. Attaching a different sink while a pair is already pending signals
  that sink once so it cannot miss readiness; repeatedly attaching the same
  sink to the same pending pair does not duplicate the signal. Signaling is
  synchronous, happens outside broadcaster and subscription locks, and the
  transport sink is restricted to a bounded wake/post operation.
- A per-connection cursor selects compact bytes for delivered frames 1 through
  9 and full bytes for delivered frame 10, repeating. Null takes and replaced
  unsent pairs do not advance the cadence.
- Broadcaster shutdown is a bounded state transition. It clears registration
  slots, closes subscriptions, and never waits for an already selected ready
  signal to return.

## Consequences

The handler can compose assets and telemetry without runtime file loading,
serialization per subscriber, callback queues, or unbounded history. A slow
or temporarily unattached transport observes the newest pair after one bounded
wake rather than a backlog. The 8 MiB packaged-asset ceiling is intentionally
stricter than a general resource archive and must be checked when the functional
alpha shell is embedded.

This decision does not implement the application handler, IOCP listener,
functional dashboard shell, or UI automation, and it does not satisfy P16 or
G16 by itself.

## Alternatives rejected

- Loading assets during lookup introduces filesystem latency and mutable
  deployment state into request handling.
- Allowing one asset to consume the complete wire ceiling leaves no guaranteed
  header budget.
- Queuing every telemetry update makes memory and callback work proportional to
  client slowness.
- Signaling only at publication time loses readiness when a sink is installed
  after the latest pair was stored.
- Advancing compact/full cadence at publication time makes dropped and replaced
  frames alter the representation sequence observed by a client.

## Evidence basis

- `.forge-codex/state/decisions/P16-004-bounded-loopback-dashboard-protocol-and-authentication.md`
- `.forge-codex/state/decisions/P16-005-bounded-dashboard-request-planning-and-response-encoding.md`
- `.forge-codex/state/decisions/P16-006-dashboard-application-and-iocp-ownership-boundaries.md`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift`
