# P16-021: Production Manager Runtime and Delivery-Safe Shutdown

Status: Accepted

Date: 2026-08-30

## Context

P16-020 established the process-owned lower dashboard transport and the
post-delivery restart edge, but the production Manager still lacked the higher
runtime that implements `IManagerRuntime`. That owner must preserve the
listener while operational APIs are paused, perform distinct-endpoint A/B
rebind with rollback, and perform destructive same-endpoint restart without
releasing the old graph prematurely.

Application construction also crossed a missing dependency seam. A listener
generation needs a concrete `DashboardConnectionApplication`, while the native
Manager runtime may not depend on the Application implementation without
reversing the layer graph.

Shutdown has a separate response-delivery constraint. A named-pipe shutdown is
dispatched before its acknowledgement is written and receipted. Publishing a
process-stop edge from `WindowsManagerRuntime::requestShutdown` would allow a
host watcher to close that pipe before its own acknowledgement was safe. The
dashboard already invokes its local Manager client only as a post-delivery
action.

## Decision

### Application factory and layer boundary

- `Dashboard::IDashboardConnectionApplicationFactory` is a transport-neutral
  construction port in the dashboard protocol layer.
- `Application::DashboardConnectionApplicationFactory` retains immutable
  resource budgets, application identity, and bearer-token policy plus borrowed
  asset, telemetry, operational, and Manager-client services. Every successful
  call creates a distinct endpoint-specific application generation.
- `ForgeConductor.Manager.Windows` is a native static layer that links
  `Manager.Host` and `Dashboard.Windows`. It depends only on the neutral
  factory interface and never links back to Application.

### Higher Manager runtime

- `WindowsManagerRuntime` serializes every public observation and mutation with
  one operation mutex. The mutex remains owned through exact lower-runtime
  drain, destruction, and successor publication, so no caller can enter the
  exclusive same-port cutover gap.
- Initial start creates an application and lower runtime, publishes the
  listener, and then applies the desired operational state. Start on an
  existing matching listener resumes it without replacing the application or
  advancing the restart counter.
- Pause disables operational APIs while preserving the dashboard and Manager
  control listener.
- Distinct host/port changes use the lower A/B rebind and retain its rollback
  guarantees.
- A same-host/port explicit restart first constructs and validates the neutral
  application while the healthy old listener remains available. It then
  pauses, drains, waits, and destroys the old Winsock/IOCP graph before it
  constructs or starts a successor lower runtime on the exclusive endpoint.
- Explicit `rebind` advances `restartCount` exactly once at invocation
  admission, including a later failed attempt. Counter exhaustion returns a
  typed conflict instead of wrapping. Successful explicit restart begins a new
  uptime epoch.
- Reconcile repairs binding or desired operational state without counting an
  explicit restart. Nonbinding settings reject endpoint changes and do not
  mutate transport ownership.
- The higher runtime stores no redundant `AppConfig` copy after a lower
  cutover. This prevents an allocation failure from reporting failure after a
  successor is already listening.
- Final shutdown is idempotent and retains the lower owner until graceful wait
  proves exact drain.

### Delivery-safe shutdown ownership

- `IManagerRuntime::requestShutdown` latches shutdown intent only. It never
  closes a client transport and does not publish a process-stop edge.
- The named-pipe server remains the owner of named-pipe shutdown sequencing. It
  writes the acknowledgement, waits for its bounded receipt, and only then
  stops ingress.
- `ManagerControllerClient` is the process-local dashboard adapter. Dashboard
  shutdown reaches it only after the HTTP response is delivered, so it
  publishes `ManagerProcessStopSignal` after the controller successfully
  latches shutdown intent.
- The composition root must retain the stop signal beyond the controller
  client and every dashboard application, and it must drain dashboard handlers
  before destroying those borrowed owners.

## Consequences

The production Manager runtime now owns exact listener state, authoritative
restart counting, rollback-preserving endpoint changes, serial same-port
replacement, and final drain. The real loopback test proves the old listener
survives a pre-cutover factory failure and that successful same- and
distinct-port transitions publish the expected application generation and
release retired ports.

Shutdown intent no longer races named-pipe acknowledgement delivery. The
dashboard keeps its post-delivery one-shot process edge, while native clients
use the pipe server's existing acknowledgement-before-stop boundary.

This checkpoint does not create the production Manager executable. Concrete
operational data, doctor, and telemetry services; terminal named-pipe worker
quiescence; transition/watchdog workers; Task Scheduler startup; live Edge
automation; and the single authoritative G16 invocation remain pending.

## Alternatives rejected

- Linking `Manager.Windows` to Application would reverse the dependency graph
  and make the native runtime construct product services directly.
- Creating the successor Winsock graph before old same-port destruction would
  compete for an exclusive endpoint. Creating only the neutral application
  early is safe and preserves the old listener on factory failure.
- Destroying the old listener before application construction would turn a
  preflight allocation or policy error into avoidable downtime.
- Publishing one undifferentiated process-stop edge inside the runtime would
  race named-pipe acknowledgement delivery.
- Retaining a duplicate active configuration after the lower transition adds
  no authoritative state and creates an exception-unsafe post-cutover commit.

## Evidence basis

- `include/ForgeConductor/Contracts/IManagerRuntime.h`
- `include/ForgeConductor/Dashboard/IDashboardConnectionApplicationFactory.h`
- `include/ForgeConductor/Application/DashboardConnectionApplicationFactory.h`
- `src/Application/DashboardConnectionApplicationFactory.cpp`
- `src/Hosts/Manager/WindowsManagerRuntime.h`
- `src/Hosts/Manager/WindowsManagerRuntime.cpp`
- `src/Hosts/Manager/ManagerControllerClient.h`
- `src/Hosts/Manager/ManagerControllerClient.cpp`
- `tests/Application/DashboardConnectionApplicationFactoryTests.cpp`
- `tests/Manager/WindowsManagerRuntimeTests.cpp`
- `tests/Manager/ManagerCompositionSupportTests.cpp`
- `.forge-codex/state/evidence/P16/manager-windows-runtime-checkpoint.json`

