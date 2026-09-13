# Forge Conductor for Windows

Forge Conductor is a native Windows control application and MCP tool server for project work with local models in LM Studio.

**Status: INTERNAL ALPHA NOT READY.** Native telemetry, Manager-owned continuity, projects, memory, MCP, tools, Settings, and the signed 0.9.5.0 package are implemented and verified outside package registration. Ordinary GUI launches now select the durable `%LOCALAPPDATA%\Forge Conductor Internal Alpha` profile and give it an independent Manager, pipe, secure-storage, memory, and continuity identity; the preserved schema-9 store is not opened. Draft [PR #25](https://github.com/flynn33/forge-conductor-windows-edition/pull/25) carries that change, side-effect-free installation preflight, and distinct install receipts. Registered install/Start/update/uninstall/reinstall acceptance remains blocked only by missing Local Machine publisher trust and the unavailable disposable lifecycle environment. Legacy schema-9 migration is owner-deferred and nonblocking.

Use [Product status](docs/STATUS.md) for verified behavior and blockers, [Roadmap](ROADMAP.md) for the current phase sequence, and [Documentation index](docs/DOCUMENTATION-INDEX.md) for active and historical guidance. The adopted execution contract is under [Alpha recovery](docs/implementation/alpha-recovery/instructions/EXECUTION.md).

## Start here

- Developers: [Build](docs/BUILD.md), [Architecture](docs/ARCHITECTURE.md), and [Focused testing](docs/TESTING.md).
- Operators: [Install](docs/INSTALL.md) and [User guide](docs/USER-GUIDE.md).
- Scope: [Alpha scope](docs/ALPHA-SCOPE.md), [Parity](docs/PARITY.md), and [Deferred work](docs/DEFERRED.md).

The target has a per-user Manager, persistent projects and memory, filesystem/Git/shell tools, LM Studio MCP deployment, real visual operational telemetry, accessible Settings, and automatic context continuity for Forge-managed runs. The native Autonomy and Continuity surfaces can start and inspect managed work and request safe pause, resume, or stop through the Manager. Ordinary LM Studio desktop MCP chats and Forge-managed API runs are different modes; connecting MCP does not enroll an existing desktop chat into managed continuity.

The current Rig renders a coalesced Manager-owned telemetry stream with a 250 ms base sample, one-second GPU/disk tiers, and five-second process/volume tiers. It shows bounded CPU/RAM/GPU and disk histories, per-logical CPU bars and frequency, supported GPU engine activity, correctly scoped DXGI memory accounting, disk throughput/IOPS and volume capacity, relevant-process rows, workflow inventory, sample age, and disconnected gaps. Retained-context headroom comes from the Manager rather than a GUI estimate. Page and project selection persist for the durable Internal Alpha profile; disposable `--alpha-root` profiles keep independent view state.

The Projects page registers an authorized folder through the Manager, displays its stable project ID and all authorized aliases, persists the selected identity, and binds it into the Autonomy run form. Memory search returns full readable records; memory writes and integrity/storage status use the existing per-project repository, with cross-project responses rejected before presentation.

Settings loads and reads back Manager, dashboard, LM Studio, logging, shell, startup, session-retention, and context values through the authenticated Manager pipe. Context thresholds have paired sliders and exact token inputs. Memory-only, continuity-only, combined, and all-project resets require exact visible confirmation text; they clear registered profile data through transactional services and never delete source folders.

## Development stack

Windows 11 x64; C++20; Visual Studio 2022/MSVC v143; Windows SDK 10.0.26100.0; CMake 3.28 or later; Windows App SDK/C++/WinRT; Windows SQLite; vcpkg JSON dependency.

```powershell
.\scripts\build.ps1 -Configuration Debug -Architecture x64 `
  -Target ForgeConductor.Cli,ForgeConductor.Manager,ForgeConductor.SessionHost
```

The GUI builds with `scripts/build.ps1 -Configuration Release -Architecture x64 -Product All`. `scripts/package.ps1 -DevelopmentSigning` creates a source-bound signed candidate under ignored `out/dist`. The current internal package is `out/dist/candidate-0.9.5.0-20260913-160324/ForgeConductor-0.9.5.0-x64.msix`, SHA-256 `157cc35181e1b940d353c38278047f522c0ac5e62bd12f24cee314417aa4b1cc`. Follow [Installer and installation](docs/INSTALL.md) for provenance and hash verification; [Installed acceptance handoff](docs/INSTALLED-ACCEPTANCE-HANDOFF.md) gives the exact trust and lifecycle procedure.

<!-- alpha-phase-review:start -->
Phase review: R6 internal Alpha completion continuation — 2026-09-13. Implementation and verification status: [Product status](docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
