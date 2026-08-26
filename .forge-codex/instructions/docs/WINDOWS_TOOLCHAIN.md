# Windows toolchain discovery and provisioning

## Required

- Windows 11.
- Visual Studio 2022 or Build Tools with Desktop development with C++.
- MSVC C++20 compiler and linker.
- Windows 11 SDK.
- CMake and CTest.
- PowerShell.
- Git.
- vcpkg.
- Windows App SDK and C++/WinRT packages compatible with the chosen target.
- MSBuild packaging support.

VS Code is an editor, not the compiler. Discover Visual Studio through `vswhere.exe` and invoke its developer environment noninteractively.

## Provisioning

Use first-party installers and package managers:

- Visual Studio Installer command line or `winget`;
- `winget` for CMake/Git/PowerShell where approved;
- vcpkg bootstrap;
- NuGet restore through MSBuild.

Pin resolved versions in manifests and evidence after discovery. Do not hardcode a guessed “latest” version in product source.

## Offline behavior

Cache packages under a repository-configurable location. If downloads are unavailable, report the exact missing component and continue source/inventory phases.
