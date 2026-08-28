# P16-013: Bounded Dashboard Connection State and Registry

Status: Accepted

Date: 2026-08-28

## Context

P16-012 established heap-stable socket, connection-event, and deadline IOCP
operations plus post-delivery executor reservations. P16-B still needed an
explicit connection lifecycle that composes those owners, a fixed registry
that routes all completion kinds, fixed overload responses, and process-owned
runtime identities and operation contexts. Logical cancellation alone could
not release a connection: late native, synthetic, and scheduler completions
may still retain exact obligations.

The scheduler can extract a due item immediately before its connection drains.
Likewise, an IOCP worker can dequeue a live deadline packet while another
worker observes that connection as drained. Registry removal therefore must
retire the exact deadline generation and serialize live reap through target
pinning before erasing the target.

## Decision

### Runtime identity and handler operations

- One process-owned runtime service allocates strictly increasing nonzero
  registration identifiers and completion keys from fixed-size,
  allocation-free staged sequences. Zero, the IOCP shutdown key, and all
  declared fixed keys are skipped permanently. Exhaustion is a retained
  integrity failure; identifiers are never returned or reused.
- Operation contexts observe cancellation and their absolute ceiling before,
  between, and after the two UUID calls. The final deadline is the lesser of
  five seconds from the final clock observation and the connection ceiling.
  No shared runtime mutex is held across the injected UUID generator, so one
  stalled call cannot serialize unrelated IOCP request admission. The
  production Windows UUID generator is stateless and supports concurrent calls.
- Closed move-only handler operations own either request preparation or one
  post-delivery action. The recording completion sink commits only complete
  typed results and preserves the previous observation if copying a failure
  diagnostic cannot complete.
- A fixed response catalog owns immutable, pre-encoded transport responses,
  including the reservation-saturation 503. The state machine never builds a
  dynamic overload response after admission has already failed.

### Connection lifecycle

- `DashboardConnectionState` is the sole owner of the per-connection socket,
  synthetic event bridge, deadline arm, parser/exchange progression,
  post-delivery reservation, SSE subscription, cancellation source, and drain
  decision. Its registry-facing interface exposes immutable identity,
  completion dispatch, shutdown, terminal observation, and a fixed snapshot.
- The lifecycle is closed from `Created` through receive, prepare, short-send
  or SSE bootstrap/frame delivery, closing, and `Drained`. One socket operation
  may be live. Partial sends retain only the exact unsent suffix, and a
  response-bearing post-delivery action reserves executor capacity before byte
  one. Send failure releases the reservation and suppresses the action.
- Header ingress has a two-second ceiling, ordinary socket work a fifteen-
  second ceiling, handler work a five-second ceiling, and SSE lifetime a
  one-hour ceiling. SSE retains one replaceable latest frame, emits at 1 or 2
  Hz, and selects the full encoding for every tenth delivered frame.
- Shutdown and fatal paths cancel owned work and close admission, but report
  `Drained` only after every native and synthetic completion obligation is
  consumed. The first event-bridge fatal is latched without recursive retry;
  a lock-contention race drains the latch immediately after the state mutex is
  released.

### Fixed registry and deadline retirement

- The registry owns exactly forty entries. It requires its one fixed deadline
  bridge before registration, obtains the exact deadline handle before
  inserting, inserts before `target.start()`, and invokes start, shutdown,
  dispatch, fatal handling, and shared-owner destruction outside the registry
  state mutex.
- Each entry retains the exact completion key, registration identifier,
  listener generation, target identity, deadline handle, and one of
  `Registered`, `Retiring`, or `Retired`. Removal first observes terminal state,
  changes the exact entry to `Retiring`, calls bridge retirement, and erases
  only after successful `Retired` finalization. A retirement failure restores
  `Registered`, retains the target and handle, records fatal shutdown, and
  cannot silently release the slot.
- Scheduler-signal-first retirement leaves one posted tombstone until exact
  IOCP reap. Retirement-first makes a late extracted scheduler signal a
  nonfatal closed-owner no-op. Concurrent unique identities may arrive out of
  numeric order; only an already-active registration identifier conflicts.
- A dedicated deadline-routing mutex is acquired before the registry state
  mutex. Live deadline consume holds it through bridge reap and target
  `shared_ptr` pin; retirement holds it through lifecycle mark, exact bridge
  retirement, and lifecycle finalization. It is released before target
  dispatch, failure shutdown, or owner destruction. An allocation-free
  snapshot bit reports this bounded in-flight routing state for diagnostics and
  deterministic lock-order validation.

### Deadline scheduler waiting

- Scheduler waits use bounded relative observations of 50 through 250
  milliseconds rather than converting an injected monotonic clock directly to
  the process steady clock. Offset or frozen injected clocks therefore cannot
  create a hot loop or a wait beyond the next bounded re-observation.
- Undefined deadline kinds are rejected before scheduler state is mutated.

## Consequences

One connection can now progress from accepted transport ownership through
bounded request preparation, short response or SSE delivery, exact
post-delivery sequencing, cancellation, and complete drain. Registry removal
cannot outrun a scheduler publication, a posted deadline tombstone, or a live
deadline packet already being reaped.

The production server composition, fixed overload accept responders, active
and retiring listener-generation integration, two-phase rebind and drain,
functional static shell, retained UI automation, and P16-C watchdog/startup
recovery remain separate work. This checkpoint does not satisfy P16 or G16.

## Alternatives rejected

- Releasing the registry entry when state first becomes terminal leaves a late
  scheduler signal without an exact owner and turns an ordinary cancellation
  race into a fatal unknown route.
- Retiring a deadline handle after erasing its target permits signal-first and
  live-reap races to observe missing registry ownership.
- Reaping a live deadline and looking up its target without routing
  serialization permits concurrent retirement to erase the entry in between.
- Holding the registry state mutex while invoking target callbacks, destroying
  shared owners, or starting a connection permits re-entrant deadlock and
  lifetime inversion.
- Serializing injected UUID generation under a shared mutex lets one stalled
  dependency delay every request beyond its cancellation or deadline ceiling.
- Constructing overload responses dynamically after reservation failure adds
  allocation and encoding failure to the only path intended to shed load.

## Evidence basis

- `src/Infrastructure/Windows/Detail/DashboardBoundedMonotonicSequence.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRuntimeServices.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRuntimeServices.cpp`
- `src/Infrastructure/Windows/Detail/DashboardHandlerOperations.h`
- `src/Infrastructure/Windows/Detail/DashboardHandlerOperations.cpp`
- `src/Infrastructure/Windows/Detail/DashboardConnectionResponseCatalog.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionResponseCatalog.cpp`
- `src/Infrastructure/Windows/Detail/DashboardConnectionState.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionState.cpp`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRegistry.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRegistry.cpp`
- `src/Infrastructure/Windows/Detail/DashboardDeadlineNotificationMailbox.h`
- `src/Infrastructure/Windows/Detail/DashboardDeadlineNotificationMailbox.cpp`
- `include/ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h`
- `src/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.cpp`
- `tests/Dashboard/DashboardConnectionRuntimeServicesTests.cpp`
- `tests/Dashboard/DashboardHandlerOperationsTests.cpp`
- `tests/Dashboard/DashboardConnectionResponseCatalogTests.cpp`
- `tests/Dashboard/DashboardConnectionStateTests.cpp`
- `tests/Dashboard/DashboardConnectionRegistryTests.cpp`
- `tests/Dashboard/DashboardDeadlineNotificationMailboxTests.cpp`
- `tests/Dashboard/WindowsDashboardDeadlineSchedulerTests.cpp`

