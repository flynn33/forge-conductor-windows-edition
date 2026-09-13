# Forge Conductor for Windows

Forge Conductor is a native Windows control application and MCP tool server for project work with local models in LM Studio.

**Status: the Windows Alpha implementation and documentation are complete, including real LM Studio context rollover and a source-bound 0.9.3.0 signed candidate; registered install/update/uninstall acceptance remains blocked by Windows machine trust.** R0–R7 delivery is merged through [PR #21](https://github.com/flynn33/forge-conductor-windows-edition/pull/21) at `a161443c974bce594ef7a655a88d02b20dec6040`. The R6 continuation repairs live managed-run persistence and rollover, records a real provider-originated handoff and productive successor, and advances the candidate to 0.9.3.0. Authentic C008/C009 migration history is still unavailable, so schema-9 owner data remains explicitly unsupported and untouched.

Use [Product status](docs/STATUS.md) for verified behavior and blockers, [Roadmap](ROADMAP.md) for the current phase sequence, and [Documentation index](docs/DOCUMENTATION-INDEX.md) for active and historical guidance. The adopted execution contract is under [Alpha recovery](docs/implementation/alpha-recovery/instructions/EXECUTION.md).

## Start here

- Developers: [Build](docs/BUILD.md), [Architecture](docs/ARCHITECTURE.md), and [Focused testing](docs/TESTING.md).
- Operators: [Install](docs/INSTALL.md) and [User guide](docs/USER-GUIDE.md).
- Scope: [Alpha scope](docs/ALPHA-SCOPE.md), [Parity](docs/PARITY.md), and [Deferred work](docs/DEFERRED.md).

The target has a per-user Manager, persistent projects and memory, filesystem/Git/shell tools, LM Studio MCP deployment, real visual operational telemetry, accessible Settings, and automatic context continuity for Forge-managed runs. The native Autonomy and Continuity surfaces can start and inspect managed work and request safe pause, resume, or stop through the Manager. Ordinary LM Studio desktop MCP chats and Forge-managed API runs are different modes; connecting MCP does not enroll an existing desktop chat into managed continuity.

The current Rig refreshes every two seconds from one authenticated Manager snapshot. CPU and RAM retain bounded history, GPU utilization stays explicitly unavailable when DXGI cannot supply it, and retained-context headroom comes from the Manager rather than a GUI estimate. Page and project selection persist for the current production profile; disposable `--alpha-root` profiles keep independent view state.

The Projects page registers an authorized folder through the Manager, displays its stable project ID and all authorized aliases, persists the selected identity, and binds it into the Autonomy run form. Memory search returns full readable records; memory writes and integrity/storage status use the existing per-project repository, with cross-project responses rejected before presentation.

Settings loads and reads back Manager, dashboard, LM Studio, logging, shell, startup, session-retention, and context values through the authenticated Manager pipe. Context thresholds have paired sliders and exact token inputs. Memory-only, continuity-only, combined, and all-project resets require exact visible confirmation text; they clear registered profile data through transactional services and never delete source folders.

## Development stack

Windows 11 x64; C++20; Visual Studio 2022/MSVC v143; Windows SDK 10.0.26100.0; CMake 3.28 or later; Windows App SDK/C++/WinRT; Windows SQLite; vcpkg JSON dependency.

```powershell
.\scripts\build.ps1 -Configuration Debug -Architecture x64 `
  -Target ForgeConductor.Cli,ForgeConductor.Manager,ForgeConductor.SessionHost
```

The GUI builds with `scripts/build.ps1 -Configuration Release -Architecture x64 -Product All`. `scripts/package.ps1 -DevelopmentSigning` creates a source-bound signed candidate under ignored `out/dist`. The current internal package is `ForgeConductor-0.9.3.0-x64.msix`, SHA-256 `9704fac82920618e19bb183e6381961a3a1880b4b4824787034e120f266c2071`. Follow [Installer and installation](docs/INSTALL.md) for provenance, hash verification, and the explicit Windows trust step.

<!-- alpha-phase-review:start -->
Phase review: R6 continuation — 2026-09-12. Implementation and verification status: [Product status](docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
