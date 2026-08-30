# P16-031: Bounded Manager Doctor and Windows Check Identities

Status: Accepted

Date: 2026-08-30

## Context

The macOS product reports one ordered Doctor document covering the application
home, database, agent catalog, Git, telemetry, installed binary, legacy
launchers, and LM Studio deployment. The Windows Manager needs the same
behavioral surface without embedding filesystem, process-discovery, database,
or deployment operations in dashboard views or transports.

Two released check identifiers contain the implementation name `swift`.
Retaining those identifiers on the C++20 Windows port would incorrectly report
the active runtime and installed artifact.

## Decision

- `ManagerDoctorService` is an outer Windows-composition implementation of
  `IDoctorService`. It borrows the platform probe, agent catalog, session
  repository, telemetry service, LM Studio deployment service, and clock. It
  copies its immutable paths, resource budgets, version, and pre-issued
  read-only LM Studio authority.
- A production `WindowsManagerDoctorPlatformProbe` returns one bounded value
  snapshot for the application home, central store, `git.exe`, installed CLI,
  and exact legacy launcher names. Any non-directory entry at an exact legacy
  name is reported, including a file reparse point. The probe retains no native
  handle or background work after inspection.
- Doctor checks are emitted in this exact order and hardness:
  `home_layout`, `sqlite_store`, `agent_catalog`, `sqlite_query`,
  `git_available`, `telemetry_native`, `telemetry_runtime`,
  `telemetry_snapshot`, `windows_binary_install` as soft,
  `no_legacy_forge_serve`, `lm_studio_native_stdio` as soft, and
  `lm_studio_mcp_plugin` as soft.
- `windows_binary_install` is the Windows-compatible replacement for
  `swift_binary_install`. `lm_studio_native_stdio` is the Windows-compatible
  replacement for `lm_studio_swift_stdio`. The semantics and serialized row
  position remain compatible while the identifiers truthfully name the native
  implementation.
- The agent catalog requires the exact ten public mandatory application agent
  identities in a duplicate-free bounded snapshot; additional unique custom
  agents remain valid. The repository check uses its read-only `quickCheck`
  boundary.
- Telemetry runtime accepts `windows-native` and
  `windows-native-realtime`. Snapshot validation forces Forge composition and
  applies the active resource budget. The pre-P17 unavailable provider remains
  a successful diagnostic report with failing hard telemetry rows and an
  overall false result; it is not converted into a failed Doctor request.
- LM Studio native stdio passes only when the expected binary is executable
  and the MCP configuration is registered. Full plugin health additionally
  requires both primary and fallback plugins.
- Cancellation, deadline expiry, and closed-service errors propagate as typed
  request failures. Other dependency errors become bounded failed rows with
  stable details and no copied dependency message or evidence identifier.
- `shutdown` closes admission and drains admitted runs without shutting down
  any borrowed dependency.

## Consequences

The dashboard and later CLI can request one deterministic, bounded Doctor
document without performing native work themselves. A degraded subsystem is
visible as diagnostic data, soft installation gaps do not make the core
runtime unhealthy, and Windows artifacts are no longer mislabeled as Swift.

The Manager composition root must supply the actual installed CLI path rather
than the running Manager executable and retain every borrowed dependency until
Doctor admission has drained. This checkpoint does not provide the production
Manager executable, real-process lifecycle, live Edge behavior, native UI
automation, or the authoritative G16 gate.

## Evidence basis

- `src/Composition/Windows/ManagerDoctorService.h`
- `src/Composition/Windows/ManagerDoctorService.cpp`
- `tests/Composition/Windows/ManagerDoctorServiceTests.cpp`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ForgeApp.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Domain/DoctorModels.swift`
