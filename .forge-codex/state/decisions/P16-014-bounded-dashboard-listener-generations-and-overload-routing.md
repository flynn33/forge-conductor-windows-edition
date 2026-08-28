# P16-014: Bounded Dashboard Listener Generations and Overload Routing

Status: Accepted

Date: 2026-08-28

## Context

P16-013 established exact connection ownership and a fixed forty-entry
registry, but accepted sockets still needed a bounded handoff, overload
transport, completion router, and rebind owner. Listener replacement also
creates a deliberate interval with one active and one retiring generation.
AcceptEx tokens, fixed 503 sends, connection entries, deadline arms, and router
registrations must remain attributable to the exact generation throughout that
interval.

A stalled overload response can otherwise pause all four AcceptEx slots in one
generation indefinitely. A connection can also reach `Drained` on a handler or
event-bridge thread without another registry callback. Finally, losing exact
router ownership while collecting a drained generation would make a later
rebind appear healthy even though an old completion or deadline target remains
registered.

## Decision

### Accepted connection handoff

- One generation-owned handoff consumes only an exact `AcceptedAndPaused`
  connection and one-shot resume token. It captures the monotonic admission
  time before short-admission arbitration so both admitted connections and
  rejected overload responses use the same absolute lifetime origin.
- Short admission, nonreusing connection identity allocation, complete owner
  construction, registry insertion-before-start, and exact AcceptEx resume are
  sequenced without a user-space queue. Every failure either transfers all
  ownership, closes it, or returns the exact token before reporting.
- The accept slot set, handoff, connection-owner factory, and listener
  generation expose and validate one immutable generation identifier,
  completion key, exact slot-set identity, and application-policy identity
  before native issue. A mismatched composition is rejected rather than
  allowing a foreign resume token, policy drift, or completion-key shadowing.

### Completion routing and fixed overload transport

- One process router owns three fixed-key slots: active listener, retiring
  listener, and the overload responder. Dynamic connection and deadline
  completions remain with the fallback registry. The fallback deadline key is
  reserved at router construction and cannot be registered by a fixed target.
- The overload owner has exactly eight heap-stable entries: four withheld
  AcceptEx tokens for each of the maximum two listener generations. Each entry
  retains accepted-socket ownership, immutable pre-encoded 503 bytes, WSABUF,
  exact OVERLAPPED, generation identity, and the one-shot resume token until
  matching IOCP reap. Partial sends reuse the same OVERLAPPED and only the
  unsent suffix.
- The fixed overload set owns one stable auxiliary deadline identity and one
  replaceable earliest-deadline scheduler arm for all eight entries. Every
  overload response expires fifteen seconds from accept. Expiry cancels and
  shuts down only due client sockets; their originating listener remains open
  and each AcceptEx token is resumed only after exact cancellation reap.
- The responder pins its exact work item and borrowed accepted-socket handle
  across synchronous IOCP association. Cancellation cannot close the socket
  until association returns, so Windows cannot reuse the numeric handle and
  associate an unrelated socket with the fixed overload key.
- Generation retirement, process shutdown, or terminal integrity failure also
  closes the exact originating listener before cancelling its sends. A partial
  completion after cancellation is abandoned without reissue. An unsolicited
  `ERROR_OPERATION_ABORTED` completion or issue-time
  `WSA_OPERATION_ABORTED` is an integrity failure, not evidence of owner
  cancellation.
- Ordinary fixed-owner shutdown publishes a nonfatal coordinator edge before
  cancelling live work. A dedicated shutdown-transition mutex serializes that
  publication and cancellation protocol across concurrent callers without
  holding the responder state mutex during coordinator re-entry. Overload-first
  and coordinator-first process shutdown therefore produce one listener
  shutdown fan-out and no false terminal classification.
- Every cancellation retains its exact work item, OVERLAPPED, immutable buffer,
  and resume token behind a five-second cancellation-reap deadline. An
  unexpected `CancelIoEx` or socket-shutdown failure closes the accepted socket
  without releasing those objects and makes the fixed owner terminal. Missing
  the exact reap deadline closes every retained client socket and invokes the
  injected process fail-fast boundary; a late exact completion can still be
  reaped safely by focused tests whose fail-fast recorder returns.
- The scheduler, mailbox, bridge, and registry ceiling is forty-four: forty
  connections, two listener generations, one overload pool, and one
  process-shutdown drain owner. The registry auxiliary table therefore has
  four exact entries.

### Listener generation and rebind ownership

- One listener generation owns its fixed completion target, auxiliary
  retirement deadline, four-slot accept owner, immutable application policy,
  generation-scoped connection control, and overload cancellation boundary.
  A shared transition gate serializes successful accept handoff with
  publication, retirement, and the retirement force-close snapshot.
- Rebind prepares and registers the successor before publication, publishes it
  as active, and moves the previous active owner to the sole retiring slot.
  The retiring generation closes admission immediately, may drain existing
  work, and at five seconds closes its exact connections and overload sends.
- The coordinator permits at most one active and one retiring owner. It rejects
  preparation while publication or collection is in progress. Collection
  removes a fully drained owner only after exact deadline and completion
  unregistration both succeed.
- A pool-local terminal error stages every affected generation identifier in a
  fixed-capacity owner. Its allocation-free pending latch reaches the
  coordinator without re-entering the shared listener transition gate, and the
  terminal callback is pumped only after that gate releases. The coordinator
  blocks collection and rebind while the latch is pending, then pins active and
  retiring owners, records fatal state, and performs one shutdown fan-out.
- The same terminal transition stages one generation-independent process-owner
  edge. A malformed fixed-key completion or fatal IOCP failure therefore fails
  closed the coordinator even when the overload set owns no live token from
  which to recover a generation identifier.
- Token return has an additional fixed provisional completion hold. It is
  installed before synchronous AcceptEx resume can expose zero accept
  ownership. If resume fails, terminal-pending is latched before the
  provisional hold is settled; if resume succeeds, the hold is settled before
  the ordinary drain edge. A concurrent last-connection callback therefore
  cannot unregister the generation in the resume-result TOCTOU window.
  Preparation sees the short provisional window as retryable conflict, while
  an actual terminal-pending latch remains nonretryable integrity failure.
- Any exact unregistration refusal is retained as a fatal coordination failure
  while the host-held owner remains tracked. Future preparation is rejected;
  the failure cannot remain a nonfatal pending collection with no future drain
  edge.
- Registry shutdown requests every auxiliary owner to stop but deliberately
  preserves its exact deadline mailbox and the shared bridge. Each listener or
  overload owner remains responsible for cancelling its arm, reaching its
  bounded drain condition, and unregistering itself. A separate routing
  finalization step refuses while any connection, auxiliary owner, posted
  notification, or retired tombstone remains, and only then shuts the bridge.
  A fatal IOCP dequeue invokes the process fail-fast boundary because neither
  completion reaps nor those watchdogs can remain deliverable.

### Drain edges

- Every connection target binds one weak, exact registry drain observer after
  deadline registration and before insertion. A transition to `Drained` on an
  IOCP, executor, SSE, or event-bridge thread publishes at most one
  `{completion key, registration identifier, generation identifier}` edge
  outside the state mutex.
- The registry pins and removes only the exact matching owner through the
  normal serialized deadline-retirement path. A change to zero connections for
  one generation emits a separate allocation-free generation edge.
- Overload work emits its ordinary generation edge only after exact work
  completion and token return. A terminal work item suppresses that edge until
  its pending latch and terminal callback have causally failed closed the
  coordinator, then emits one post-terminal ownership recheck. Listener
  collection therefore needs no polling and cannot infer drain from unrelated
  generation activity.

## Consequences

Accepted sockets now have a closed, bounded route from AcceptEx through either
registered connection ownership or best-effort fixed 503 delivery. One active
and one retiring listener can overlap without sharing tokens, completion keys,
deadlines, application policy, or connection counts. Stalled overload clients
cannot indefinitely suppress listener admission, and off-registry drain edges
cannot leave dead connection entries consuming capacity.

The production dashboard/server composition and its process-wide graceful
shutdown owner remain next. Current fixed-owner `beginShutdown()` closes work
immediately; the required production policy must close admission, SSE, receive,
and preparation immediately while allowing only already-prepared short final
sends to drain for at most five seconds. Static shell assets, retained UI
automation, and P16-C watchdog/startup recovery also remain pending. This
checkpoint does not satisfy P16 or G16.

## Alternatives rejected

- Allocating one timer or worker per overload response adds eight timer owners
  and unnecessary threads when a single replaceable earliest arm is sufficient.
- Returning an AcceptEx token before exact send reap permits stable OVERLAPPED
  storage to be reused while the kernel can still publish the old operation.
- Treating every aborted operation as expected cancellation hides completion
  loss and permits a corrupted fixed owner to continue accepting connections.
- Closing an entire active listener when one overload client expires converts
  client backpressure into avoidable service loss.
- Removing a drained generation before both exact router unregistrations
  succeed loses the only owner capable of safely receiving a late packet.
- Polling connection and overload counts adds latency and races around the
  exact zero transition already known to the ownership boundary.

## Evidence basis

- `include/ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h`
- `src/Infrastructure/Windows/Detail/DashboardAcceptedConnectionHandoff.h`
- `src/Infrastructure/Windows/Detail/DashboardAcceptedConnectionHandoff.cpp`
- `src/Infrastructure/Windows/Detail/DashboardIocpCompletionRouter.h`
- `src/Infrastructure/Windows/Detail/DashboardIocpCompletionRouter.cpp`
- `src/Infrastructure/Windows/Detail/DashboardOverloadResponderSet.h`
- `src/Infrastructure/Windows/Detail/DashboardOverloadResponderSet.cpp`
- `src/Infrastructure/Windows/Detail/DashboardListenerGeneration.h`
- `src/Infrastructure/Windows/Detail/DashboardListenerGeneration.cpp`
- `src/Infrastructure/Windows/Detail/DashboardListenerGenerationCoordinator.h`
- `src/Infrastructure/Windows/Detail/DashboardListenerGenerationCoordinator.cpp`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRegistry.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRegistry.cpp`
- `src/Infrastructure/Windows/Detail/DashboardConnectionState.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionState.cpp`
- `tests/Dashboard/DashboardAcceptedConnectionHandoffTests.cpp`
- `tests/Dashboard/DashboardIocpCompletionRouterTests.cpp`
- `tests/Dashboard/DashboardOverloadResponderSetTests.cpp`
- `tests/Dashboard/DashboardListenerGenerationTests.cpp`
- `tests/Dashboard/DashboardConnectionRegistryTests.cpp`
- `tests/Dashboard/DashboardConnectionStateTests.cpp`
