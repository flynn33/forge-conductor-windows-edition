# P16-008: Closed Dashboard Application Dispatch and JSON Boundary

Status: Accepted

Date: 2026-08-27

## Context

P16-006 assigns one parsed dashboard request to a transport-neutral application
handler and P16-007 closes response, asset, and SSE publication ownership. The
application boundary still needs exact route dispatch, injectable operational
and telemetry ports, macOS-compatible JSON projections, mutation decoding, and
a response-safe mapping for arbitrary dependency errors.

The native transport must not understand product routes, inspect application
results, retain request storage, or call manager shutdown before the response
is delivered. Conversely, application code must not own sockets, Winsock
operations, completion keys, deadlines, or connection state.

## Decision

### Application models and ports

- `IDashboardTelemetrySource` is the only dashboard path to the existing
  telemetry producer. It provides bounded health and latest-value observations
  and one seeded capacity-one subscription. The handler never starts a
  collector or installs a competing consumer.
- `IDashboardOperationalService` exposes the released doctor, agent, session,
  audit, diagnostics, prune, and administrative close operations without JSON,
  HTTP, database, filesystem, or platform types.
- Application projections carry explicit collection and aggregate-text caps.
  Encoders reject a value outside those caps; they never truncate a collection
  or source string. Open-session projections also reject terminal session
  states rather than relying on a name alone.
- Listener identity, endpoint, bearer policy, and resource budgets are copied
  into one noncopyable `DashboardConnectionApplication` for that immutable
  listener generation. Injected services remain owned by the composition root.

### Request dispatch and failure closure

- `DashboardRequestPlanner` remains the only request policy and route
  classifier. A rejection is encoded immediately and no dependency is called.
  HEAD rejections advertise the exact corresponding JSON representation length
  but contain no body or post-delivery action.
- Every released non-streaming route is dispatched through a closed enum
  switch. Static paths use the canonical decoded path; required shell mappings
  are treated as composition invariants. A missing ordinary static asset keeps
  the released 404 `Not Found` behavior.
- Status composition calls operational status, telemetry health, and manager
  status in that order with the same bounded operation context and stops at the
  first failure. The response retains the exact top-level `manager` and
  `service_active` fields added by the macOS dashboard server.
- All telemetry aliases obtain one immutable observation per request. A null
  snapshot from a nominally successful source is rejected as an internal
  dependency failure before dereference.
- Dependency messages and evidence identifiers never cross the HTTP boundary.
  A closed error mapper emits fixed public status, code, and message values.
  Request-body failures, SSE admission failures, and response-encoding failures
  retain distinct public classifications.

### JSON and mutation compatibility

- Application JSON uses a streaming bounded writer with a maximum body of
  2,080,768 bytes. It validates strict UTF-8, NUL exclusion, finite numbers,
  UTC bounds, collection semantics, and aggregate text before publishing one
  complete document.
- Status, doctor, agents, sessions, audit, diagnostics, prune, close, and
  manager-shutdown responses preserve the released macOS field names and
  optional/null behavior. Agent bodies remain omitted. Session, audit, and
  presence timestamps use second-resolution Internet-date-time values, matching
  the released macOS `ISO8601.string(from:)` encoder.
- Session-close bodies are limited to 64 KiB and 16 JSON nesting levels, reject
  duplicate or unknown members, require one canonical UUID `session_id`, and
  cap the summary at 4,000 Unicode scalar values. Missing or non-string
  `session_id` retains the released `session_id required` response; an omitted
  summary uses `Closed from dashboard`.
- Manager settings mutations continue through the existing strict manager JSON
  codec. Manager status and control responses reuse its exact schema.

### Delivery and shutdown

- Preparing `/api/manager/shutdown` performs no manager call. It returns the
  exact acknowledgement with the enum-only `RequestManagerShutdown` action.
- `executePostDelivery` is the only application path that calls
  `IManagerClient::requestShutdown`. `None`, shutdown, and invalid enum values
  form a closed result contract; manager failures propagate after delivery and
  cannot change the already-sent acknowledgement.
- Cache policy remains owned by `DashboardHttpResponseEncoder`. Asset dispatch
  does not add a second `Cache-Control` header.

## Consequences

The upcoming IOCP transport can treat every application result as an opaque,
move-only exchange. It needs only to deliver bytes, attach an SSE ready sink,
and schedule an approved action after a successful final send. Route logic,
JSON schema, mutation parsing, dependency error redaction, and shutdown ordering
are independently testable without Winsock.

Concrete telemetry and operational adapters, the bounded handler executor,
IOCP listener, rebind/drain behavior, functional static shell, and UI automation
remain pending. This decision and its focused tests do not satisfy P16 or G16.

## Alternatives rejected

- Dispatching routes in the transport duplicates policy and lets socket state
  leak into application behavior.
- Reflecting dependency messages exposes internal paths, evidence identifiers,
  and potentially sensitive text.
- Truncating sessions, audit entries, or diagnostics silently changes product
  meaning and can hide an invalid producer.
- Calling manager shutdown during response preparation races the acknowledgement
  against destruction of its own transport and listener.
- Re-serializing or inspecting JSON in the IOCP layer weakens the closed
  application-to-transport boundary.

## Evidence basis

- `.forge-codex/state/decisions/P16-006-dashboard-application-and-iocp-ownership-boundaries.md`
- `.forge-codex/state/decisions/P16-007-bounded-dashboard-assets-and-sse-publication.md`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/DashboardServer.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/OperationalRoutes.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/ManagerRoutes.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Domain/DoctorModels.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Domain/JSONSupport.swift`

