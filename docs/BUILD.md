# Build on Windows

## Requirements

- Windows 11 x64
- Visual Studio 2022 with MSVC v143, Desktop C++, UWP/XAML C++ tools, and Windows SDK 10.0.26100.0
- CMake 3.28 or later
- PowerShell 7
- vcpkg with `VCPKG_ROOT` set

Windows App SDK 2.4.0 and C++/WinRT 3.0.260818.1 are pinned by the native app project. The build restores and verifies the pinned Forsetti source revision before configuring; it refuses a source-tree mismatch.

## Complete Release build

From the repository root:

```powershell
$env:VCPKG_ROOT = '<vcpkg-root>'
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product All -Parallel 4
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product Backend -Parallel 4
./scripts/test.ps1 -Configuration Release -Architecture x64 -Parallel 4
./scripts/Run-Static-Gates.ps1
```

`-Product All` builds and stages the WinUI application and required sibling services under `out/app/x64/Release`. `-Product Backend` builds the complete CMake graph, including all tests. The staging manifest binds executable hashes to the exact source commit and tree.

For a clean reconfiguration, add `-Fresh` to the first build only. Build outputs, restored inputs, certificates, packages, logs, tokens, and machine-specific paths remain outside tracked source.

## Packaging

After committing the product inputs and rebuilding from that commit:

```powershell
# Local validation only
./scripts/package.ps1 -DevelopmentSigning

# Production signing
./scripts/package.ps1 -PfxPath '<certificate.pfx>' -PfxPassword '<password>'

# Production signing with App Installer update metadata
./scripts/package.ps1 -PfxPath '<certificate.pfx>' -PfxPassword '<password>' `
  -UpdateBaseUri 'https://downloads.example.com/forge-conductor'
```

The package step rejects dirty release inputs, mismatched staging provenance, missing release files, Debug CRTs, unresolved manifest placeholders, bad signatures, and payload hash drift.
