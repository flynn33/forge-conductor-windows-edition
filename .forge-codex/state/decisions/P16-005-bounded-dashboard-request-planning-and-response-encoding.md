# P16-005: Bounded Dashboard Request Planning and Response Encoding

Status: Accepted

Date: 2026-08-27

## Context

P16-004 fixes the dashboard authentication, request grammar, route, response,
and resource boundaries. The parser, request policy, and route catalog are pure
components, but a transport must not independently reinterpret their results.
Static-resource suffixes and stream queries need narrowly scoped decoding after
policy succeeds, and response framing needs a single bounded implementation
before live socket and handler code is added.

The macOS dashboard accepts `hz` and legacy `interval` stream parameters. It
normalizes `hz` to 1 through 60 Hz, normalizes legacy intervals to 0.016 through
0.1 seconds before taking the reciprocal, and defaults to 20 Hz. The Windows
resource profiles deliberately cap actual telemetry delivery at 1 or 2 Hz.

Windows path lookup adds case folding, reserved device names, alternate
separators, drive syntax, alternate data streams, and Unicode-normalization
aliases that do not exist in the same form on macOS. The alpha asset inventory
uses lowercase ASCII HTML, CSS, JavaScript, and JSON names, so a narrower
canonical static namespace preserves every current asset without exposing
platform aliases.

## Decision

### Planner boundary and precedence

- `DashboardRequestPlanner` is the only pure boundary that combines an
  accepted HTTP request, `DashboardRequestPolicy`, `DashboardRouteCatalog`,
  route-specific decoding, operational-service state, and the selected
  resource budgets.
- It applies request policy first. Host, bearer, mutation media type, origin,
  and fetch-metadata failures therefore win before unknown paths,
  deliberately unavailable routes, stopped-service state, static decoding, or
  stream-query decoding.
- After policy succeeds, route dispositions map deterministically:
  method mismatch to 405 `method_not_allowed`, stopped operational service to
  503 `service_stopped`, and both unknown and deliberately unavailable routes
  to the indistinguishable 404 `not_found` response. A 405 includes an `Allow`
  header only when the catalog has a nonempty allowed-method set.
- The immutable plan is exactly one of rejection, ordinary dispatch,
  static-resource dispatch, or server-sent-events dispatch. It owns all
  decoded values, exposes only const snapshots, holds no platform resource,
  and borrows no request memory.
- Exceptions are caught at the planner boundary and converted to the typed 500
  `internal_failure` rejection.

### Static-resource namespace

- Static decoding runs only after exact raw `/static/` classification. It
  receives `DashboardHttpRequest::path()`, not the complete target, so a public
  cache-buster query does not become part of the filesystem name.
- The raw target is capped at 8,192 bytes, the decoded relative path at 1,024
  bytes, and each segment at 255 bytes. Decoding is one pass and produces an
  owned canonical relative path.
- Encoded separators, backslashes, NUL/control bytes, malformed escapes,
  invalid UTF-8, empty or dot segments, traversal, drive and UNC forms,
  alternate data streams, trailing dots, Windows device stems, uppercase
  spellings, and non-ASCII spellings are rejected before any infrastructure
  path resolution.
- Only lowercase ASCII `.html`, `.css`, `.js`, and `.json` resources are
  admitted. Their MIME types come from a closed mapping. Every decoder failure
  becomes the same 404 response, so the transport does not disclose path
  validation detail.

### Stream-query compatibility and resource cap

- Stream decoding receives the route catalog's query classification and the
  same complete raw target. A mismatch is rejected rather than reclassified.
- The value grammar is the strict JSON number grammar. Percent encoding,
  controls, missing values, extra components, non-finite values, and numeric
  underflow or overflow are rejected with 400 `invalid_query`.
- The decoder first preserves the macOS normalization contract: default 20 Hz,
  explicit `hz` clamped to 1 through 60, or legacy `interval` clamped to 0.016
  through 0.1 seconds before reciprocal conversion. It then caps delivery to
  the exact selected Windows profile ceiling of 1 or 2 Hz.
- The fixed-size result records source, compatibility clamping, and resource
  capping. It never stores or copies untrusted target text.

### HTTP response framing

- Complete HTTP/1.1 responses are encoded in one exact-size allocation after
  checked measurement. The 2,097,152-byte ceiling includes both head and body;
  the head has an independent 16,384-byte ceiling.
- A distinct HEAD-response model carries the representation length but no body
  bytes. A 204 response omits `Content-Length` entirely; other HEAD responses
  advertise the bounded representation length while emitting no representation
  bytes.
- The encoder owns status text, content type and length, connection behavior,
  cache policy, and security headers. Callers may add at most eight headers
  from the fixed `Allow`, `Content-Disposition`, `Content-Language`, `ETag`,
  `Last-Modified`, `Location`, `Retry-After`, and `WWW-Authenticate` allowlist.
  Names and values have explicit byte ceilings and control/injection checks.
- Complete responses use `Connection: close`, deterministic
  `Content-Length`, `no-store`, `nosniff`, no-referrer, same-origin resource
  policy, and the P16-004 content-security policy. No CORS header is emitted.
- SSE bootstrap is a distinct type and encoder path. It omits
  `Content-Length`, uses keep-alive and no-cache/no-transform semantics, and
  cannot accidentally be encoded as a complete response.
- Unsupported status codes, invalid metadata, forbidden bodies, header
  overflow, and total response overflow return typed encoding errors instead
  of partially encoded bytes.

## Consequences

The later socket listener and route handlers receive one closed plan and cannot
change authentication precedence, decode a route under a different identity,
or invent response framing. Static lookup can be implemented beneath an
application-owned asset store using only canonical relative paths. SSE runtime
work receives a validated rate selection but still must enforce connection
admission, latest-value mailboxes, deadlines, and bounded shutdown.

The lowercase ASCII static namespace is intentionally narrower than the
general Windows filesystem. A future product-owned asset with another naming
or MIME requirement must update this contract and its tests before packaging;
the live server may not widen it implicitly.

This decision does not prove handler behavior, socket transport, the DPAPI
bearer store, the static shell, SSE delivery, two-phase listener replacement,
or P16-C lifecycle recovery. Those remain separate P16 work.

## Alternatives rejected

- Letting each handler apply policy and routing separately permits precedence
  drift and decoded-route confusion.
- Passing a complete static target to the path decoder treats a cache-buster as
  a filename or encourages ad hoc query stripping below the policy boundary.
- Resolving arbitrary UTF-8 asset names against Windows paths creates case,
  normalization, device, separator, and reparse ambiguities without an alpha
  feature requirement.
- Using `strtod`-style permissive numeric parsing accepts spellings outside the
  documented query grammar and makes trailing text handling easy to miss.
- Allocating a response and checking its size afterward can temporarily exceed
  the resource contract and cannot safely support a hard aggregate ceiling.
- Sharing complete-response framing with SSE risks emitting a content length
  or close semantics on a live stream.

## Evidence basis

- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/state/decisions/P16-004-bounded-loopback-dashboard-protocol-and-authentication.md`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-codex/state/baseline/p02-telemetry-dashboard-inventory.json`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Dashboard/TelemetryRoutes.swift`

