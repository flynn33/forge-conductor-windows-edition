# P16-015: Fixed IOCP Authority and Graceful Shutdown Cutover

Status: Accepted

Date: 2026-08-28

## Context

P16-014 established bounded active and retiring listener generations, an
eight-entry overload responder, exact two-router collection, and a forty-four
owner deadline ceiling. Production composition still needs one unambiguous
completion-key authority and a process shutdown path that preserves only
short responses already being sent while retaining every native and synthetic
completion owner through exact reap.

Listener completion keys cannot be drawn from the dynamic connection sequence:
two generations can overlap, a late callback can retain a shared generation
pin after router unregistration, and reusing a slot before that final pin is
destroyed would make lifetime proof depend on timing. Shutdown also cannot use
the existing hard generation and registry fan-out because it would cancel the
very final sends that the five-second graceful policy permits.

## Decision

### Fixed completion-key authority

- One immutable process authority declares exactly four nonzero, distinct
  completion keys: fallback deadline, overload response, listener slot A, and
  listener slot B. Production values are one through four. The IOCP worker
  shutdown value is never valid for any role.
- Runtime services accept the complete authority in production composition.
  Dynamic connection keys permanently skip all four values. Connection,
  fixed-role, and auxiliary deadline identities share one monotonically
  increasing registration identifier sequence whose values are never reused.
- Fixed identity allocation issues an identity only; it is not a key lease.
  Listener composition must first acquire one of the two listener slots from a
  bounded RAII lease pool. A third simultaneous acquisition is a retryable
  conflict.
- Listener A and B are interchangeable lease slots, not permanent active and
  retiring roles. A lease is moved into its concrete generation and is the
  first declared member, so C++ reverse member destruction returns the key
  only after every other generation dependency is gone.
- No coordinator path explicitly releases a listener key and no path polls a
  shared-pointer reference count. Exact deadline and completion unregister
  failures retain the generation. Router, deadline, transition, and external
  callbacks retain their shared generation pin, so the generation destructor
  is the only safe release edge.

### Runtime and connection cutoff

- Runtime services expose one mutex-linearized, idempotent shutdown edge. Once
  closed, they reject every new dynamic identity, fixed identity, auxiliary
  identity, and operation context with `TransportClosed`. Clock and operational
  observations remain available to bounded draining owners.
- UUID generation remains outside the admission mutex. A final mutex-protected
  check prevents an in-flight context from crossing shutdown without allowing
  an injected UUID source to block the cutover.
- A handler completion not incorporated into its connection state before the
  runtime shutdown edge is not an already-prepared response. The connection
  closes without sending complete, fallback, SSE, or post-delivery work.
- Connection graceful shutdown retains only `SendingComplete`, whose immutable
  response bytes and one native send are already owned. It may reissue only an
  unsent suffix after a valid partial completion. Preparation, receive, SSE,
  queued handler results, and post-delivery actions close immediately. Existing
  hard shutdown remains the exact escalation path.

### Registry zero edge and process ordering

- The registry closes connection admission and fans graceful callbacks outside
  its mutex. It emits one nonblocking zero-connection edge when graceful
  shutdown observes or reaches exact zero.
- Graceful and hard registry fan-outs are serialized independently of the
  registry data mutex. Once hard fan-out starts, no target may receive a later
  graceful callback. Graceful-to-hard escalation remains valid; a same-thread
  hard escalation latches hard state and prevents the outer graceful loop from
  invoking another target.
- The zero observer is bound before listener admission and is retained strongly
  by process composition until final routing drain. The registry stores a weak
  reference to avoid hidden process ownership. Missing that lifetime at the
  exact edge is an integrity failure and invokes the process fail-fast boundary
  instead of silently hanging.
- The zero callback is a latch only. It may re-enter for observations but may
  not destroy registry, kernel, executor, runtime, listener, overload, bridge,
  or scheduler dependencies because the outer registry fan-out can still hold
  shared target pins.
- Production shutdown first arms the independent five-second
  `ShutdownDrain` deadline. It then closes runtime admission and serializes
  handler-executor admission closure before listener and overload admission
  closure through the shared listener transition gate. Only after that fence
  and ordinary overload shutdown does it ask the registry to publish or reach
  zero. This ordering prevents an in-flight handoff from owning an uncommitted
  deadline registration after the sole zero edge.
- Natural zero or the hard deadline only advances a process latch; final
  teardown waits for exact listener, overload, executor, connection,
  auxiliary deadline, bridge tombstone, and IOCP worker drain. At five seconds,
  the existing hard generation and registry paths cancel every retained final
  send and preserve operation storage until reap.

## Consequences

The fixed completion-key namespace is explicit and dynamic connections cannot
collide with it. Two listener generations can overlap without timing-based key
reuse, while a drained generation whose callback is still executing continues
to own its slot. Registration identities remain unique even when a fixed slot
is later reused.

Shutdown now has a precise application-level cutover: only bytes already owned
by a complete-response send can finish, and no late handler result can become
eligible while native listener handoff is being quiesced. Weak observer misuse
fails closed, and its callback cannot perform unsafe reentrant destruction.

The concrete listener factory, graceful coordinator fan-out, process
`ShutdownDrain` owner, static shell, retained UI automation, and P16-C recovery
remain required. This decision and its focused tests do not satisfy P16 or
G16.

## Alternatives rejected

- Allocating listener keys from the dynamic sequence makes fixed router
  composition depend on allocation history and cannot express the two-slot
  overlap bound.
- Treating fixed identity issuance as an implicit lease allows two prepared
  owners to claim one completion role before either reaches the router.
- Returning a slot immediately after router unregister ignores callbacks that
  already pinned the old generation outside the router lock.
- Calling hard registry or generation shutdown at the initial cutover cancels
  allowed final response sends.
- Publishing registry zero before listener handoff quiesces can miss an
  uncommitted deadline owner that never changes the registered connection
  count.
- Letting the zero callback tear down process dependencies is unsafe when it is
  invoked reentrantly from a target still pinned by registry fan-out.
- Allowing graceful and hard registry fan-outs to race can deliver graceful
  shutdown after a target has already observed hard shutdown, weakening the
  one-way process lifecycle.

## Evidence basis

- `src/Infrastructure/Windows/Detail/DashboardFixedIocpKeyAuthority.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRuntimeServices.h`
- `src/Infrastructure/Windows/Detail/DashboardListenerCompletionKeyLease.h`
- `src/Infrastructure/Windows/Detail/DashboardListenerGeneration.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionState.h`
- `src/Infrastructure/Windows/Detail/DashboardConnectionRegistry.h`
- `tests/Dashboard/DashboardFixedIocpKeyAuthorityTests.cpp`
- `tests/Dashboard/DashboardConnectionRuntimeServicesTests.cpp`
- `tests/Dashboard/DashboardListenerCompletionKeyLeaseTests.cpp`
- `tests/Dashboard/DashboardListenerGenerationTests.cpp`
- `tests/Dashboard/DashboardConnectionStateTests.cpp`
- `tests/Dashboard/DashboardConnectionRegistryTests.cpp`
