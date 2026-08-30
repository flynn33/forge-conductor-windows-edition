# P16-019: Embedded Bounded Dashboard Shell

Status: Accepted

Date: 2026-08-29

## Context

The P16 dashboard protocol, authenticated route catalog, bounded static asset
store, application JSON codecs, and SSE publication were already native C++
surfaces. The Windows product still needed concrete browser assets for the
telemetry and manager/control shells without adding an installed Node runtime,
filesystem asset reads on each request, or unbounded browser request and
render ownership.

The owner decision in `OWNER-002-alpha-release-qualification-scope.md` defers
bespoke visual polish until after alpha. It does not defer any dashboard
feature, the production Manager composition path, live Edge validation, or the
complete native C++ UI Automation coverage required for alpha.

## Decision

### Packaged assets and native ownership

- The served bundle contains exactly six immutable assets: telemetry and
  control HTML, one shared stylesheet, and authentication, telemetry, and
  control JavaScript modules.
- Stable integer identifiers map those files into native Windows `RCDATA`.
  `WindowsDashboardStaticAssetBundle` loads each resource from the current
  process module, enforces the existing per-asset and aggregate store bounds,
  copies it once into the application-owned immutable asset store, and maps
  root/index and control/manager shell aliases without runtime filesystem
  access.
- Missing, empty, oversized, malformed-path, or unreadable resources fail
  closed with typed domain errors. Resource handles are not retained after the
  one-time copy.
- Checked-in JavaScript is the shipped asset. Neutral TypeScript source is
  retained as a paired maintenance artifact with an explicit matching contract
  marker. CMake does not transpile it, and neither the installed application
  nor its runtime acquires a Node dependency. A development-only `node
  --check` invocation validates the three shipped JavaScript modules.

### Browser request and stream bounds

- Authentication accepts a 64-hex bearer token only from the URL fragment,
  stores it in per-tab `sessionStorage`, removes the fragment through
  `history.replaceState`, and sends it only in the `Authorization` header.
- One response owner keeps the request timeout and caller abort signal active
  through successful or error JSON body consumption. JSON reads use a fixed
  2,080,768-byte buffer and reject overflow or malformed UTF-8 before parsing.
- Streaming fetch transfers the same response ownership through the complete
  reader callback. Visibility and page-hide cancellation therefore abort the
  actual body read rather than only the header fetch.
- Telemetry retains a capacity-one pending render frame, fixed row and event
  limits, bounded reconnect backoff, one abortable fallback refresh, and one
  visibility-aware silence watchdog. A stream without telemetry frames
  requests the existing single-flight `/api/live` fallback at 2.5 seconds and
  aborts for reconnect after more than 5 seconds. The 500-millisecond watchdog
  is cleared whenever the page hides, unloads, or the stream owner exits.

### Functional parity and presentation

- The telemetry shell exposes the required system strip, load trace, CPU and
  GPU detail, storage, orchestration, MCP server and tool, agent-session,
  process, and live-feed surfaces through semantic HTML and bounded DOM
  replacement.
- A missing or malformed collection renders `Unavailable`; a valid empty
  collection renders an explicit no-items message. MCP servers, tools, agent
  sessions, and live-feed entries therefore no longer conflate unknown state
  with a known zero result.
- The control shell retains manager status for lifecycle command enablement
  and also reads the bounded composite `/api/status` surface for service,
  open-session, agent, presence, runtime, and runtime-pressure KPIs.
- Settings reload has one promise owner and one GET site. Settings, operational
  refresh, and mutation paths cross-guard one another, so repeated clicks
  cannot create concurrent settings reads or overlap a read with a mutation.
- Conventional responsive Windows-oriented styling, keyboard focus,
  forced-color behavior, reduced-motion behavior, semantic tables, and live
  status regions satisfy the owner-approved alpha presentation scope. Bespoke
  styling and decorative polish remain deferred by OWNER-002.

## Consequences

The application now has a bounded, authenticated static shell whose packaged
bytes, aliases, source contracts, route references, request ownership, and
collection-state semantics are deterministic native-test surfaces. The
installed runtime remains C++ and Windows-native; checked-in JavaScript is
static presentation code and does not own server business logic.

The resource-bearing native tests prove that the six files can be compiled as
`RCDATA`, loaded from a process module, copied into the bounded store, and
inspected for the stated static contracts. They do not prove live browser
timing, cancellation, accessibility, rendering, navigation, or end-to-end API
behavior. The production Manager executable does not yet exist and therefore
does not yet compile `DashboardAssets.rc` or construct the bundle in its
composition root. Those production wiring and live Edge checks remain P16
work, and the retained packaged-app C++ UI Automation suite remains later
alpha work. This checkpoint does not satisfy P16 or G16.

## Alternatives rejected

- Loading dashboard files from disk for every request adds deployment drift,
  mutable runtime inputs, and repeated I/O that the immutable store avoids.
- Compiling TypeScript during installation or shipping Node would violate the
  approved native runtime boundary and add an unnecessary production
  dependency.
- Releasing request ownership after response headers leaves JSON parsing and
  stream reads outside timeout and visibility cancellation.
- Independent settings reload promises allow repeated UI input to create
  concurrent GETs and stale form replacement.
- Treating every empty-looking collection as unavailable destroys the
  observable distinction between missing telemetry and a valid zero result.
- Inventing absent telemetry values as zeros would misrepresent the current
  native codec rather than preserve honest parity.
- Adding bespoke visual treatments before functional production composition
  would conflict with OWNER-002's alpha sequencing.

## Evidence basis

- `include/ForgeConductor/Infrastructure/Windows/WindowsDashboardStaticAssetBundle.h`
- `src/Infrastructure/Windows/WindowsDashboardStaticAssetBundle.cpp`
- `src/Hosts/Manager/Resources/Dashboard/DashboardAssets.rc`
- `src/Hosts/Manager/Resources/Dashboard/DashboardResourceIds.h`
- `src/Hosts/Manager/Resources/Dashboard/index.html`
- `src/Hosts/Manager/Resources/Dashboard/control.html`
- `src/Hosts/Manager/Resources/Dashboard/dashboard.css`
- `src/Hosts/Manager/Resources/Dashboard/auth.ts`
- `src/Hosts/Manager/Resources/Dashboard/auth.js`
- `src/Hosts/Manager/Resources/Dashboard/telemetry.ts`
- `src/Hosts/Manager/Resources/Dashboard/telemetry.js`
- `src/Hosts/Manager/Resources/Dashboard/control.ts`
- `src/Hosts/Manager/Resources/Dashboard/control.js`
- `tests/Dashboard/WindowsDashboardStaticAssetBundleTests.cpp`
- `tests/Dashboard/DashboardStaticShellContractTests.cpp`
- `.forge-codex/state/evidence/P16/dashboard-static-shell-checkpoint.json`
