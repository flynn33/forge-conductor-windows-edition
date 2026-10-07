<p align="center">
  <img src="images/banner-architecture.jpg" alt="Forge Conductor engineering lines" width="100%">
</p>

# Developer Setup

This page is for building and validating the native product. For normal installation, use [Guided Setup](Guided-Setup).

## Requirements

| Tool | Required version or workload |
| --- | --- |
| OS | Windows 11 x64 |
| Visual Studio | 2022, MSVC v143, Desktop C++, UWP/XAML C++ tools |
| Windows SDK | 10.0.26100.0 |
| CMake | 3.28 or later |
| PowerShell | 7 recommended |
| vcpkg | `VCPKG_ROOT` set |
| Windows App SDK | 2.4.0, restored by the app project |
| C++/WinRT | 3.0.260818.1 |

The build refuses an unsupported generator, toolset, SDK, architecture, Forsetti revision, or dependency graph.

## Clone

```powershell
git clone https://github.com/flynn33/forge-conductor-windows-edition.git
Set-Location .\forge-conductor-windows-edition
```

## Complete x64 Release validation

```powershell
$env:VCPKG_ROOT = '<vcpkg-root>'
.\scripts\build.ps1 -Configuration Release -Architecture x64 -Product All -Parallel 4
.\scripts\build.ps1 -Configuration Release -Architecture x64 -Product Backend -Parallel 4
.\scripts\test.ps1 -Configuration Release -Architecture x64 -Parallel 4
.\scripts\Run-Static-Gates.ps1
```

`-Product All` builds and stages the WinUI application with its shipping siblings. `-Product Backend` builds the complete native CMake graph and tests. The staging manifest binds executable hashes to the exact source commit and tree.

## Create an installable package

Commit the intended product inputs and rebuild from that exact commit before packaging.

```powershell
# Local validation package
.\scripts\package.ps1 -DevelopmentSigning

# Production signing
.\scripts\package.ps1 -PfxPath '<certificate.pfx>' -PfxPassword '<password>'
```

The package step rejects dirty release inputs, source/provenance mismatch, missing payload files, Debug runtimes, unresolved manifest placeholders, signature failures, and hash drift.

## Repository map

```text
src/ForgeConductor.App/                 WinUI 3 host and views
src/Hosts/Manager/                      per-user Manager process
src/Hosts/Cli/                          CLI and stdio MCP host
src/Hosts/SessionHost/                  logical-session host
src/Manager/                            Manager protocol and dispatcher
src/Mcp/                                103-tool catalog, protocol, router
src/Infrastructure/Windows/             native OS services
src/Persistence/Windows/                winsqlite3 repositories/migrations
include/ForgeConductor/                 public layer headers
tests/                                  native test matrix
packaging/                              MSIX inputs and installer metadata
scripts/                                build, test, package, validation
docs/                                   authoritative source documentation
```

## Build boundaries

- Native product logic is C++20.
- The WinUI host is a Microsoft C++/WinRT MSBuild project coordinated by the canonical build script.
- The installed application contains no Python, Electron, Qt, Java, .NET application layer, or Node runtime.
- Optional dashboard TypeScript is compiled into static assets; it does not ship a Node runtime or own business logic.
- Forsetti is restored at a pinned revision and consumed through public contracts.

## Developer profiles

Normal launches use:

```text
%LOCALAPPDATA%\Forge Conductor
```

For disposable isolated validation only:

```powershell
ForgeConductorApp.exe --alpha-root C:\path\to\empty-test-profile
```

Never run development tests against the operator's production profile.

## Before pushing

1. Inspect `git status` and preserve unrelated work.
2. Run the complete relevant Release build and test matrix.
3. Run static gates.
4. Package from clean, source-bound inputs when release artifacts changed.
5. Record exact commands, configuration, exit codes, hashes, and remaining limitations.

**Next:** [Architecture](Architecture) · [Toolchain](Toolchain) · [Validation Gates](Validation-Gates)
