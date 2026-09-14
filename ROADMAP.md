# Forge Conductor roadmap

## 1.1 product recovery

The 1.1 line restores routine native operation: independent telemetry and command scheduling, automatic Manager startup, delayed-readback edit preservation, plumbing-free project/run controls, and synchronized primary/fallback/CLU LM Studio registration. It also adds the exact four-tool continuity-control role without claiming desktop-chat takeover that has not been observed end to end.

## 1.0 completion

The 1.0 release line closes the gaps left after the internal Windows validation cycle:

- production version and package identity;
- ordinary use of `%LOCALAPPDATA%\Forge Conductor`;
- exact compatibility with released central schema 9 and immutable C001–C009 migration history;
- signed MSIX distribution plus optional App Installer update manifest;
- complete WinUI GUI and native runtime build;
- repeatable Windows CI and production-signing automation;
- full Release test, static-gate, package-integrity, and simulated lifecycle validation.

The earlier R0–R7 plan is complete and retained under [historical delivery records](docs/implementation/alpha-recovery/). It no longer defines release readiness.

## After 1.1

Future work is versioned enhancement, not unfinished 1.0 scope. Candidate themes include additional processor architectures, more provider integrations, expanded performance telemetry, and Store distribution. Any such work must preserve the native C++20/WinUI architecture, Manager ownership boundaries, project isolation, and migration compatibility.
