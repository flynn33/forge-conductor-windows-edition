# Forge Conductor for Windows

Forge Conductor is a native Windows control application and MCP tool server for project work with local models in LM Studio.

**Status: Windows Alpha implementation is active; the signed engineering package is not yet an accepted installed candidate.** R1 provides the Manager-owned ordinary Responses/tool loop and native Autonomy/Continuity controls. R2 is merged and adds Manager-owned Windows CPU/RAM/process telemetry, DXGI capability reporting, a typed native snapshot, live Rig gauges and histories, operational status cards, activity latency/timeline values, and telemetry-backed detail pages. R3 now provides real project registration/selection and persistent project-memory workflows in the native Projects page. Native visual/accessibility walkthroughs remain blocked until a native UI control surface is available. The remaining operating pages, accessible maintenance settings, installer lifecycle, and live productive rollover acceptance remain in R3–R7.

Use [Product status](docs/STATUS.md) for verified behavior and blockers, [Roadmap](ROADMAP.md) for the current phase sequence, and [Documentation index](docs/DOCUMENTATION-INDEX.md) for active and historical guidance. The adopted execution contract is under [Alpha recovery](docs/implementation/alpha-recovery/instructions/EXECUTION.md).

## Start here

- Developers: [Build](docs/BUILD.md), [Architecture](docs/ARCHITECTURE.md), and [Focused testing](docs/TESTING.md).
- Operators: [Install](docs/INSTALL.md) and [User guide](docs/USER-GUIDE.md).
- Scope: [Alpha scope](docs/ALPHA-SCOPE.md), [Parity](docs/PARITY.md), and [Deferred work](docs/DEFERRED.md).

The target has a per-user Manager, persistent projects and memory, filesystem/Git/shell tools, LM Studio MCP deployment, real visual operational telemetry, accessible Settings, and automatic context continuity for Forge-managed runs. The native Autonomy and Continuity surfaces can start and inspect managed work and request safe pause, resume, or stop through the Manager. Ordinary LM Studio desktop MCP chats and Forge-managed API runs are different modes; connecting MCP does not enroll an existing desktop chat into managed continuity.

The current Rig refreshes every two seconds from one authenticated Manager snapshot. CPU and RAM retain bounded history, GPU utilization stays explicitly unavailable when DXGI cannot supply it, and retained-context headroom comes from the Manager rather than a GUI estimate. The last selected page is retained for the current Windows user.

The Projects page registers an authorized folder through the Manager, displays its stable project ID and all authorized aliases, persists the selected identity, and binds it into the Autonomy run form. Memory search returns full readable records; memory writes and integrity/storage status use the existing per-project repository, with cross-project responses rejected before presentation.

## Development stack

Windows 11 x64; C++20; Visual Studio 2022/MSVC v143; Windows SDK 10.0.26100.0; CMake 3.28 or later; Windows App SDK/C++/WinRT; Windows SQLite; vcpkg JSON dependency.

```powershell
.\scripts\build.ps1 -Configuration Debug -Architecture x64 `
  -Target ForgeConductor.Cli,ForgeConductor.Manager,ForgeConductor.SessionHost
```

The GUI builds with `scripts/build.ps1 -Configuration Release -Architecture x64 -Product All`. `scripts/package.ps1 -DevelopmentSigning` creates a signed engineering distribution under ignored `out/dist`; it is development evidence rather than a public download or accepted installed Alpha.

<!-- alpha-phase-review:start -->
Phase review: R2 — 2026-09-12. Implementation and verification status: [Product status](docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
