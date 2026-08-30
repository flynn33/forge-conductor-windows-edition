# P16-023: Bounded Task Scheduler Adapter and Terminal COM Ownership

Status: Accepted

Date: 2026-08-30

## Context

The exact per-user startup policy in P16-022 requires a concrete Windows Task
Scheduler adapter. Task Scheduler is a synchronous COM boundary: calls can
block, cancellation is thread-affine, returned interfaces and automation values
carry explicit ownership, and task names remain mutable Windows state. The
public Manager startup service is synchronous, but concurrent callers must not
create unbounded native work or release operation ownership while COM is still
using request data.

Task registration also has an ownership race. Even after Forge Conductor opens
and classifies an owned task, another process running as the same user can
replace that named task before a subsequent name-based update or deletion.
Task Scheduler does not expose a conditional mutation primitive tied to the
previously inspected registration object.

## Decision

### One bounded MTA owner

- `WindowsManagerStartupService` owns one `ManagerStartupComWorker`, and that
  worker owns one dedicated `std::jthread` initialized as a COM multithreaded
  apartment with COM call cancellation enabled.
- Admission is fixed at one active request and one FIFO successor. Additional
  requests fail with a retryable limit error, and duplicate operation IDs fail
  as conflicts.
- The caller's stop token is linked to an operation-owned stop source before a
  request becomes visible to the worker. The internal context is revalidated
  while holding the admission lock, and both the original and linked tokens are
  checked at the dispatch linearization point.
- Deadline expiry claims the terminal cancellation result while holding the
  same state lock used by handler completion. A handler result cannot win after
  the caller has observed the hard deadline.
- Active cancellation requests the linked token and pulses `CoCancelCall`
  immediately and every 50 milliseconds while the caller retains terminal
  ownership. Queued cancellation completes without dispatch.
- Cancellation and shutdown each have a five-second drain bound. If an
  in-process COM call does not quiesce within that bound, the process fails
  stopped with `std::terminate`; it never detaches a thread that still owns COM
  interfaces or request memory. This is a deliberate availability-versus-
  ownership tradeoff.
- A Detail-only admission gate exists solely to make the pre-dispatch
  cancellation ordering deterministic in native tests. Production composition
  cannot supply it through the public service.

### Exact native projection

- The concrete platform resolves current-user identity and canonical task
  definition on every operation. It owns all COM interfaces, BSTRs, and
  VARIANTs through typed RAII wrappers and converts every HRESULT at the native
  boundary.
- Projection covers the closed P16-022 ownership, principal, privilege,
  trigger, repetition, action, hidden-window, settings, idle, network,
  maintenance, enablement, state, and last-run surfaces. Missing or unsupported
  required interfaces fail instead of weakening the comparison.
- Native XSD durations are normalized only when they are exact fixed whole
  seconds. Calendar or fractional forms are preserved as bounded text and
  therefore remain visible drift.
- Task Scheduler's timezone-less last-run `DATE` is interpreted using the
  machine's local-time convention and converted to UTC before it crosses the
  infrastructure boundary.

### Mutation boundary

- Every mutating platform call reconnects, reopens, projects, and classifies
  the current named task immediately before mutation.
- Creation uses `TASK_CREATE`; owned repair uses `TASK_UPDATE`. The adapter
  never uses `TASK_CREATE_OR_UPDATE`.
- Enabling and starting require an exact owned task. Disabling and removal may
  also act on owned drift where the application policy explicitly permits it.
  A foreign task is never intentionally updated, enabled, started, or removed.
- Every successful mutation is re-inspected by the handler and must satisfy its
  exact postcondition before `changed=true` is returned.

## Consequences

The service has one explicit native lifetime owner, a fixed queue bound, a
deterministic cancellation linearization point, and no detached COM work.
Callers receive terminal cancellation or deadline results only after native
ownership has drained. A blocked Task Scheduler server can therefore terminate
the Manager after five seconds rather than leave an unowned background call;
the external watchdog must recover the process.

The immediate ownership recheck narrows accidental and ordinary races but
cannot make Task Scheduler's name-based mutation conditional. A malicious or
compromised process already running as the same Windows user could replace the
task in the remaining recheck-to-mutation window, causing an update or removal
to act on that replacement. Network isolation does not remove this local
same-user risk. Under OWNER-002, additional hardening for this race is deferred
until after alpha; foreign-state tests and exact pre-mutation checks remain
mandatory for alpha.

The startup service is not a watchdog. It registers and reconciles durable
startup state; a separately owned P16 watchdog is responsible for recovery.
This checkpoint does not complete P16 or invoke G16.

## Alternatives rejected

- Running COM on arbitrary caller threads would make apartment state,
  cancellation, and shutdown ownership dependent on every caller.
- An unbounded executor would violate the Manager resource budget and allow a
  blocked COM server to accumulate work.
- Returning immediately after cancellation would detach live COM ownership
  from the request and service lifetime.
- `TASK_CREATE_OR_UPDATE` would silently overwrite a same-name registration
  without proving whether creation or owned replacement was intended.
- Treating a partial native projection as exact would allow unmodeled launch
  behavior to evade drift classification.

## Evidence basis

- `include/ForgeConductor/Infrastructure/Windows/WindowsManagerStartupService.h`
- `src/Infrastructure/Windows/WindowsManagerStartupService.cpp`
- `src/Infrastructure/Windows/Detail/ManagerStartupComWorker.h`
- `src/Infrastructure/Windows/Detail/ManagerStartupComWorker.cpp`
- `src/Infrastructure/Windows/Detail/WindowsManagerStartupComHandler.cpp`
- `src/Infrastructure/Windows/Detail/WindowsTaskSchedulerStartupPlatform.cpp`
- `src/Infrastructure/Windows/Detail/TaskSchedulerDurationCodec.cpp`
- `src/Infrastructure/Windows/Detail/UniqueBstr.h`
- `src/Infrastructure/Windows/Detail/UniqueComInterface.h`
- `src/Infrastructure/Windows/Detail/UniqueVariant.h`
- `tests/Manager/ManagerStartupComWorkerTests.cpp`
- `tests/Manager/WindowsManagerStartupComHandlerTests.cpp`
- `tests/Manager/WindowsManagerStartupServiceTests.cpp`
- `.forge-codex/state/evidence/P16/manager-startup-task-scheduler-checkpoint.json`
