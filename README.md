# Forge Conductor for Windows
A native Windows control application and MCP tool server for project work with local models in LM Studio.

**Status: Alpha under implementation — not yet qualified as an installable working Alpha.**
The current build evidence and remaining work are in [Status](docs/STATUS.md).
The [source audit](docs/reference/AUDIT.md) explains the baseline.
The delivery plan is [Roadmap](ROADMAP.md), and scope is [Alpha scope](docs/ALPHA-SCOPE.md).

## Start here
- Developers: [Build](docs/BUILD.md), [Architecture](docs/ARCHITECTURE.md), [Focused testing](docs/TESTING.md).
- Operators: [Install](docs/INSTALL.md) and [User guide](docs/USER-GUIDE.md).
- Feature comparison: [macOS parity](docs/PARITY.md) and [Deferred work](docs/DEFERRED.md).

The intended Alpha has a WinUI 3 desktop interface, a per-user manager, project memory,
filesystem and shell tools, LM Studio MCP deployment, and automatic context continuity for Forge-managed runs.
Normal LM Studio desktop MCP chats and Forge-managed API runs are different modes.
Do not assume that connecting MCP enrolls an existing desktop chat into managed continuity.

## Development stack
Windows 11 x64; C++20; Visual Studio 2022/MSVC v143; Windows SDK 10.0.26100.0;
CMake 3.28 or later; Windows App SDK/C++/WinRT; Windows SQLite; vcpkg JSON dependency.
See Build for discovery and exact dependency restoration. Newer toolchains are not required just to finish Alpha.

```powershell
# Existing native backend entry point; restore the pinned Forsetti input first.
.\scripts\build.ps1 -Configuration Debug -Architecture x64 `
  -Target ForgeConductor.Cli,ForgeConductor.Manager,ForgeConductor.SessionHost
```

The native GUI now builds with `scripts/build.ps1 -Configuration Release -Architecture x64 -Product All`.
`scripts/package.ps1 -DevelopmentSigning` creates a signed engineering distribution under `out/dist`.
See [Status](docs/STATUS.md) for runtime and installation limitations; this is not the accepted Alpha.
There is no all-historical-gates prerequisite to ordinary development.

Source status is not a release download. Publish an installer link only after the distribution is built and checked.
