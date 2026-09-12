# Forge Conductor for Windows

Forge Conductor is a native Windows control application and MCP tool server for project work with local models in LM Studio.

**Status: Windows Alpha implementation is active; the signed engineering package is not yet an accepted installed candidate.** The merged foundation includes the C++20/WinUI 3 host, Manager attachment, provider settings, real LM Studio Responses bootstrap transport, context-only continuity policy, native project/MCP/tool services, and signed engineering MSIX packaging. Manager-owned ordinary runs, complete native telemetry and workflows, accessible maintenance settings, installed lifecycle, and live productive rollover remain in the R0–R7 delivery plan.

Use [Product status](docs/STATUS.md) for verified behavior and blockers, [Roadmap](ROADMAP.md) for the current phase sequence, and [Documentation index](docs/DOCUMENTATION-INDEX.md) for active and historical guidance. The adopted execution contract is under [Alpha recovery](docs/implementation/alpha-recovery/instructions/EXECUTION.md).

## Start here

- Developers: [Build](docs/BUILD.md), [Architecture](docs/ARCHITECTURE.md), and [Focused testing](docs/TESTING.md).
- Operators: [Install](docs/INSTALL.md) and [User guide](docs/USER-GUIDE.md).
- Scope: [Alpha scope](docs/ALPHA-SCOPE.md), [Parity](docs/PARITY.md), and [Deferred work](docs/DEFERRED.md).

The target has a per-user Manager, persistent projects and memory, filesystem/Git/shell tools, LM Studio MCP deployment, real visual operational telemetry, accessible Settings, and automatic context continuity for Forge-managed runs. Ordinary LM Studio desktop MCP chats and Forge-managed API runs are different modes; connecting MCP does not enroll an existing desktop chat into managed continuity.

## Development stack

Windows 11 x64; C++20; Visual Studio 2022/MSVC v143; Windows SDK 10.0.26100.0; CMake 3.28 or later; Windows App SDK/C++/WinRT; Windows SQLite; vcpkg JSON dependency.

```powershell
.\scripts\build.ps1 -Configuration Debug -Architecture x64 `
  -Target ForgeConductor.Cli,ForgeConductor.Manager,ForgeConductor.SessionHost
```

The GUI builds with `scripts/build.ps1 -Configuration Release -Architecture x64 -Product All`. `scripts/package.ps1 -DevelopmentSigning` creates a signed engineering distribution under ignored `out/dist`; it is development evidence rather than a public download or accepted installed Alpha.

<!-- alpha-phase-review:start -->
Phase review: R0 — 2026-09-12. Implementation and verification status: [Product status](docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
