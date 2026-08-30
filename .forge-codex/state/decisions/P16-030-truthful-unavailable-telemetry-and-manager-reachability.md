# P16-030: Truthful Unavailable Telemetry and Manager Reachability

Status: Accepted

Date: 2026-08-30

## Context

The production Manager composition planned in P16 requires an
`ITelemetryService`, while the native collectors, histories, and rendering
pipeline belong to P17 through P19. Starting the Manager before those phases
must not fabricate observations or describe an inactive stream as healthy.

The public `/ping` page also described telemetry and continuous collectors as
reachable without consulting telemetry health. That made a successful Manager
listener indistinguishable from a healthy telemetry subsystem.

## Decision

- `UnavailableTelemetryService` is an outer Windows-composition adapter used
  only until the P17 production telemetry service replaces it.
- `start` admits a degraded Manager startup after enforcing cancellation and
  deadline precedence. It creates no worker, callback, queue, or snapshot.
- `health` succeeds with the stable Windows telemetry identity while setting
  `ok` to false and `mode` to `unavailable`. The `ui` field identifies the
  planned native dashboard delivery surface; it does not override the explicit
  unavailable state.
- Forced and ordinary samples fail with retryable
  `HostCapabilityUnavailable`, `latest` is empty, pending work is zero, and a
  supplied consumer is immediately released without being retained or called.
- `stop` is an idempotent no-op because this adapter owns no asynchronous or
  native resource lifetime.
- The dashboard telemetry projection preserves the false health state and
  exposes zero measured frequency with no running stream. It does not own the
  telemetry service lifecycle.
- `/ping` proves only that the Manager dashboard endpoint is reachable. The
  authenticated `/api/health` endpoint remains the telemetry-readiness source,
  and snapshot or stream routes retain typed unavailable failures.

## Consequences

P16 can compose and exercise a truthful degraded Manager before native
telemetry exists. A healthy HTTP listener is no longer presented as evidence
that collectors or SSE delivery are active, and no consumer capture or sample
can accumulate behind an unavailable provider.

P17 must replace this adapter with the bounded native collector composition.
This checkpoint does not prove live telemetry, the production Manager
executable, real-process lifecycle, Edge behavior, native UI automation, or the
authoritative G16 gate.

## Evidence basis

- `src/Composition/Windows/UnavailableTelemetryService.h`
- `src/Composition/Windows/UnavailableTelemetryService.cpp`
- `src/Application/DashboardConnectionApplication.cpp`
- `tests/Composition/Windows/UnavailableTelemetryServiceTests.cpp`
- `tests/Application/DashboardConnectionApplicationTests.cpp`
- `tests/Application/DashboardTelemetrySourceTests.cpp`
