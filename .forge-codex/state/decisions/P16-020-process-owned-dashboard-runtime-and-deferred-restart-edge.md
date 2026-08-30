# P16-020: Process-Owned Dashboard Runtime and Deferred Restart Edge

Status: Accepted

Date: 2026-08-30

## Context

The dashboard transport primitives already proved bounded Winsock, IOCP,
deadline, connection, generation, overload, and shutdown ownership in
isolation. P16 still required one production lower-runtime owner that composes
those primitives before admission, preserves immutable listener bindings, and
can prove exact teardown from a real loopback listener.

Manager restart also has a Windows-specific ownership constraint. The listener
uses `SO_EXCLUSIVEADDRUSE`; closing the listening socket does not prove that
accepted connections have released the endpoint. A restart requested by the
dashboard cannot synchronously drain and rebind from its own request handler,
because the requesting connection remains registered until the post-delivery
operation returns. That creates a self-drain cycle and can either deadlock or
drop the acknowledgement.

## Decision

### Lower dashboard runtime

- `WindowsDashboardListenerGenerationFactory` publishes one immutable
  configuration/application snapshot. Prepared generations retain the exact
  snapshot and do not observe later publication changes.
- `WindowsDashboardRuntime` owns the complete fixed-key, lease, Winsock,
  registry, router, IOCP, deadline, scheduler, admission, overload,
  generation, fail-fast, and shutdown-drain graph through final wait.
- The shutdown drain is installed before listener admission. Partial
  construction cleanup requires exact overload drain, exact auxiliary and
  fixed-route removal, scheduler shutdown, and finalized deadline routing; a
  failed proof terminates while the complete graph is still retained.
- A background drain does not publish the lower runtime as `Drained` or hide
  its application identity. The process owner observes `ShuttingDown` until
  `wait()` completes the join and releases the retained binding.
- Initial-start failures clear unpublished binding state. Distinct-endpoint
  A/B rebind retains the previous binding on candidate failure and publishes
  the candidate only after the coordinator succeeds.
- Same-endpoint lower-runtime rebind returns a typed non-retryable conflict
  before mutating factory or active ownership. Destructive same-port restart
  is a higher Manager-runtime operation that must fully drain and destroy the
  old lower runtime before constructing its successor.

### Deferred restart edge

- `/api/manager/restart` returns an explicit accepted/restarting document and
  carries `RequestManagerRestart` as a one-shot post-delivery action. It no
  longer reports an old `ManagerStatus` as if restart had completed.
- The connection reserves bounded post-delivery capacity before sending byte
  one. The restart edge is published only after the full acknowledgement has
  been delivered; failed or cancelled sends suppress it.
- `ManagerProcessRestartSignal` is the process-owned capacity-one transition
  edge. Idle becomes Pending once, concurrent or in-flight requests coalesce,
  the sole Manager worker claims Pending as InFlight without polling, and
  completion returns it to Idle. Close is permanent and wakes the worker.
  Closing during an active operation rejects new work but preserves the
  in-flight proof until the worker acknowledges completion.
- `ManagerControllerClient::requestRestart` publishes or coalesces that signal
  and never invokes the controller inline. A separate Manager transition
  worker will pin the controller, create a fresh bounded operation context,
  and execute `ManagerControlAction::Restart` outside dashboard handler and
  IOCP ownership.
- The browser validates the accepted response, retains one reconnect timer,
  and performs at most twelve one-second manager-status probes separated by
  500 milliseconds. It navigates only after observing `running` and a strictly
  newer restart count, and otherwise exposes a bounded manual-refresh state.
  Page hide aborts the active probe while preserving the accepted restart
  intent and advances an ownership generation; persisted page show resumes
  the same bounded intent. Every resumed probe rechecks both generation and
  expected restart count before it can mutate the page or navigate. If no
  restart is active, a separate bounded restoration owner probes the listener,
  restores settings and refresh ownership, and then re-arms automatic refresh.

## Consequences

The lower transport is now an independently testable production composition
unit with real socket evidence, exact snapshot observations, and idempotent
drain. The dashboard restart request can acknowledge safely without entering
the old generation's drain cycle, and repeated operator clicks cannot create
an unbounded transition queue.

This checkpoint deliberately does not claim same-port restart completion. The
production `WindowsManagerRuntime`, its transition worker, and the real
acknowledgement-to-successor loopback test remain required. That runtime must
set operational state false, request and await bounded old-runtime drain,
destroy the old graph, create and start a successor on the same endpoint, and
publish running only after successor admission. Failure after destructive
cutover must remain honestly non-listening and fully owned for watchdog
recovery. P16 and G16 remain open.

## Alternatives rejected

- Treating same-endpoint rebind as a successful metadata publication does not
  restart the HTTP control plane and loses macOS behavior.
- Performing drain/rebind inside the post-delivery handler still waits on the
  connection whose handler is executing and therefore preserves the cycle.
- Enabling `SO_REUSEADDR` weakens deterministic endpoint ownership and does not
  supply an exact current-user cutover proof.
- Returning the pre-restart status misrepresents an accepted operation as a
  completed transition.
- An unbounded restart queue turns repeated UI input into repeated transport
  destruction and violates the process resource contract.

## Evidence basis

- `include/ForgeConductor/Contracts/IManagerServices.h`
- `include/ForgeConductor/Dashboard/DashboardPreparedExchange.h`
- `src/Application/DashboardConnectionApplication.cpp`
- `src/Hosts/Manager/ManagerControllerClient.cpp`
- `src/Hosts/Manager/ManagerProcessRestartSignal.cpp`
- `src/Infrastructure/Windows/Detail/WindowsDashboardListenerGenerationFactory.cpp`
- `src/Infrastructure/Windows/Detail/WindowsDashboardRuntime.cpp`
- `tests/Application/DashboardConnectionApplicationTests.cpp`
- `tests/Dashboard/DashboardConnectionStateTests.cpp`
- `tests/Dashboard/WindowsDashboardListenerGenerationFactoryTests.cpp`
- `tests/Dashboard/WindowsDashboardRuntimeTests.cpp`
- `tests/Manager/ManagerCompositionSupportTests.cpp`
- `tests/Manager/ManagerProcessRestartSignalTests.cpp`
- `.forge-codex/state/evidence/P16/dashboard-process-runtime-deferred-restart-checkpoint.json`

Microsoft platform behavior references:

- https://learn.microsoft.com/en-us/windows/win32/winsock/so-exclusiveaddruse
- https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse
