# P16-004: Bounded Loopback Dashboard Protocol and Authentication

Status: Accepted

Date: 2026-08-27

## Context

P16-B must preserve the macOS dashboard, manager-control, operational, and
telemetry routes while replacing Network.framework with native Windows
networking. The dashboard is intentionally reachable by a browser, but it also
contains state-changing manager and session operations. Loopback binding alone
does not identify the current user or prevent another local process from
calling those routes.

The macOS parser already bounds headers at 32 KiB and bodies at 1 MiB, rejects
transfer encodings and duplicate headers, validates the Host header, requires
same-origin JSON mutations, caps live SSE streams at 32, and sends security
headers without permissive CORS. Several Windows transport values and the
browser authentication bootstrap were not specified. Those choices must be
fixed before socket, router, or static-shell code can diverge.

The owner deferred security-only hardening and bespoke UI polish until after
alpha. Current-user authentication, loopback-only binding, bounded work, and
the complete functional dashboard remain alpha behavior rather than optional
hardening.

## Decision

### Endpoint and authentication boundary

- The listener binds only the configured literal `127.0.0.1` or `::1`.
  `localhost`, wildcard, hostname, mapped-address, and automatic fallback
  bindings are rejected. A port collision returns the typed `Conflict` error;
  the manager never selects another port or enables address reuse.
- Every request must carry the exact active endpoint in `Host`:
  `127.0.0.1:<port>` or `[::1]:<port>`. A missing, alternate, or malformed Host
  value is rejected with 403 before routing.
- A dashboard-specific random 256-bit bearer is stored using DPAPI CurrentUser
  under `manager.dashboard.bearer.v1`. It is distinct from the named-pipe nonce
  and is never accepted in a query parameter, cookie, log, diagnostic field,
  or project-memory record.
- `/`, `/index.html`, `/control`, `/manager`, `/ping`, and the raw `/static/`
  prefix are the complete public bootstrap allowlist. Every other target,
  including every `/api/*`, unknown, encoded-route spelling, and deliberately
  unavailable tool route, requires `Authorization: Bearer
  <64-lowercase-hex-characters>`. The candidate and expected token are compared
  without data-dependent early return. Missing or invalid credentials receive
  401 and `WWW-Authenticate: Bearer`.
- The native manager opens a URL whose fragment carries the bearer. The static
  shell copies it to session storage, removes the fragment with
  `history.replaceState` before issuing a request, and uses authenticated
  `fetch` calls. SSE uses streaming `fetch`; query tokens and unauthenticated
  `EventSource` are prohibited.
- A successful applied binding change returns the new canonical dashboard URL
  (`http://127.0.0.1:<port>/` or `http://[::1]:<port>/`) while the old listener
  drains. The shell reads its bearer from the old origin's session storage and
  navigates to `<new-url>#token=<encoded-current-bearer>` only when both
  `applied` and `bind_changed` are true. The new shell repeats fragment removal
  into its own origin-scoped session storage. No cross-origin fetch or CORS
  exception is used, and the server never returns the bearer in JSON.

### Request grammar and policy

- The transport accepts one strict HTTP/1.1 request per connection and emits
  `Connection: close`, except for an accepted SSE stream. HTTP pipelining,
  absolute-form targets, fragments, malformed percent escapes, embedded NUL,
  obsolete line folding, duplicate headers, `Transfer-Encoding`, and bytes
  after the declared body are rejected.
- Bytes preceding the `CRLF CRLF` delimiter are capped at 32,768. A request has
  at most 64 headers, an 8,192-byte origin-form target, and a 1,048,576-byte
  body. Header text must be strict UTF-8; names use the HTTP token grammar and
  values may not contain disallowed controls. `Content-Length` is one unsigned
  decimal value with no sign, separator, or whitespace.
- Incomplete input is distinct from rejected input. A closed stream with an
  incomplete header or body is rejected with 400. Header/count/target overflow
  maps to 431 or 414, and declared body overflow maps to 413.
- `POST`, `PUT`, `PATCH`, and `DELETE` require `application/json`; the only
  permitted media-type parameter is `charset=utf-8`. If present, `Origin` must
  equal the exact active origin. If present, `Sec-Fetch-Site` must be
  `same-origin` or `none`. Violations map to 415 or 403 before routing.
- Policy and routing compare the same undecoded raw path after removing only
  the query delimiter and suffix. Route identity is never percent-decoded.
  Static-resource and query handlers may decode only their already-classified
  suffix/value; they reject encoded separators, NUL/control bytes, invalid
  UTF-8, and `.` or `..` segments. Thus an encoded route spelling cannot be
  reinterpreted after the authentication decision.
- `OPTIONS` is not a supported route method and receives 405. No response emits
  `Access-Control-Allow-Origin` or another permissive CORS header.

### Response, connection, and SSE bounds

- A non-streaming encoded response is capped at 2,097,152 bytes. The router
  reports a typed bounded error instead of allocating or writing beyond that
  limit. Static resources are individually subject to the same limit.
- At most 40 loopback connections are admitted: 32 live SSE streams and eight
  short-lived requests. Admission beyond either bound receives 503 without an
  unbounded queue.
- Native socket work uses four fixed I/O workers with asynchronous socket
  operations; no connection or SSE stream owns a thread. Header ingress has a
  two-second deadline, a complete non-streaming request has a five-second
  deadline, an idle short connection has a fifteen-second ceiling, shutdown
  drains for at most five seconds, and one SSE connection lives for at most one
  hour.
- Telemetry follows the selected Windows resource profile at 1 to 2 Hz. The
  telemetry producer and each SSE consumer use a latest-value mailbox of
  capacity one: a newer frame replaces an unsent frame. Slow consumers never
  create a callback or frame backlog. Keepalive comments do not raise the
  telemetry sampling rate.
- Complete responses use deterministic lengths and `no-store`; SSE uses its
  macOS-compatible `no-cache, no-transform` framing. Both use `nosniff`,
  same-origin resource policy, no-referrer policy, and a self-contained
  same-origin content-security policy. Exceptions are converted to typed HTTP
  errors at the router/transport boundary.

### Route and lifecycle ownership

- The canonical route inventory remains
  `.forge-codex/state/baseline/p02-telemetry-dashboard-inventory.json`. Static,
  telemetry, manager, `/api/status`, and SSE routes remain available while the
  operational service is paused. Other operational `/api/*` routes return 503
  with `service_stopped`. `/api/tools/call` remains unavailable and returns 404
  after authentication and request policy succeed.
- Dashboard manager routes use an injected `IManagerClient` over the existing
  current-user named pipe. They do not hold or call the concrete controller and
  cannot create a second manager composition path.
- The dashboard uses a dedicated JSON codec rather than exposing the named-pipe
  envelope. Manager status objects contain exactly `ok`, `manager`, `state`,
  `desired_running`, `http_listening`, `service_active`, `pid`, `started_at`,
  `uptime_sec`, `restart_count`, `last_error`, `auto_restart`,
  `watchdog_interval_sec`, `open_browser_on_start`, `dashboard`, `home`, and
  `version`. `dashboard` contains `host`, `port`, `url`, and
  `refresh_interval_sec`; absent optional values are JSON null.
- Manager settings objects contain exactly `ok`, `dashboard`, `manager`,
  `sessions`, `shell`, and `log_level`. Their nested keys are
  `dashboard.{host,port,refresh_interval_sec}`,
  `manager.{auto_restart,watchdog_interval_sec,open_browser_on_start}`,
  `sessions.idle_ttl_sec`, and `shell.default_timeout_sec`.
- A settings mutation accepts a JSON object with optional boolean `apply`
  (default true) and either a nested `settings` object or the settings patch at
  the top level. Patch values use the same nested keys as the settings object;
  unknown or duplicate JSON keys and wrong JSON types are rejected. Its success
  response is the settings object plus `applied`, `bind_changed`, and the exact
  manager `status` object.
- The application and named-pipe contracts carry one atomic typed settings
  update outcome containing settings, `applied`, `bindingChanged`, and status.
  `IManagerController`, the manager protocol, and `IManagerClient` preserve that
  outcome from the serialized mutation; the dashboard codec only maps its
  fields. A pre-read/update/post-read adapter sequence is prohibited because a
  concurrent native client could make the reconstructed metadata describe a
  different operation.
- A binding update starts and validates the new listener before publishing it.
  The prior listener then closes admission and remains retained while its
  in-flight requests drain for at most five seconds. This allows a settings
  response to leave on the old connection, avoids self-join, and keeps the old
  listener active if the replacement bind fails.
- The dashboard shutdown route writes the complete acknowledgement
  `{"ok":true,"message":"Manager shutting down","state":"stopping"}` before
  its post-delivery action asks `IManagerClient` to shut down the manager. The
  response path therefore cannot be torn down by the shutdown it initiates.
- The Windows manager runtime owns listener, telemetry, watchdog, maintenance,
  and retirement lifetimes. The pure parser, policy, codec, and router own no
  Win32 handles and depend only on contracts and immutable request snapshots.

### Alpha presentation scope

The alpha ships a functional, keyboard-accessible static shell for every
dashboard feature and state. It uses conventional semantic controls and
precompiled static assets with no installed Node runtime. Custom visual design,
decorative imagery, animation, and pixel-level refinement remain deferred by
OWNER-002; their deferral does not remove routes, controls, errors, telemetry,
or UI-automation coverage.

## Consequences

The HTTP parser and request policy can be exhaustively unit-tested without a
socket. The native listener has exact memory, connection, deadline, thread, and
stream ceilings before implementation begins. Browser access remains usable
without putting a bearer in HTTP request targets, while every dynamic route has
a current-user secret boundary in addition to loopback scope.

Two-phase listener replacement temporarily retains the old listener only for
bounded in-flight delivery. It requires asynchronous socket ownership and a
retirement reaper, but prevents settings and shutdown requests from destroying
their own response channel.

The 2 MiB response ceiling may require an endpoint to return a bounded error or
use its existing stream form when a maximum-size history cannot be represented
in one document. It does not permit silent truncation.

## Alternatives rejected

- Treating loopback as authentication permits any process in the user session
  to invoke dashboard controls.
- Reusing the pipe nonce couples independent transports and increases the
  effect of one credential disclosure.
- A query bearer leaks through browser history, copied URLs, and request logs.
- Cookie authentication adds state and cross-site request behavior without a
  product need.
- A thread per connection or stream exceeds the constrained 32-thread manager
  budget before pipe, telemetry, watchdog, and maintenance owners are counted.
- Unbounded headers, response serialization, connections, streams, or pending
  frames contradict the project resource contract.
- Stopping the old listener synchronously during a settings request can close
  that request's own connection or self-join its worker.
- A placeholder polished dashboard would spend alpha scope on deferred visual
  work while leaving transport and behavior incomplete.

## Evidence basis

- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/state/decisions/P16-001-single-owner-manager-and-current-user-control-plane.md`
- `.forge-codex/instructions/architecture/PROCESS_MODEL_AND_IPC.md`
- `.forge-codex/instructions/architecture/SECURITY.md`
- `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-codex/state/baseline/p02-telemetry-dashboard-inventory.json`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/DashboardHTTPRequest.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/ManagerRoutes.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Manager/ManagerNode.swift`
