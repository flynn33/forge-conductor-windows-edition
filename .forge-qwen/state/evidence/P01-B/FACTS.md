# P01-B: Provision missing first-party tools noninteractively - Facts

## VERIFIED (probe.ps1, exit 0; raw output: probe-output.txt)

- **Windows SDK 10.0.26100.0**: include dir present at `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0` (subdirs: cppwinrt, shared, ucrt, um, winrt). Resolves P01-A UNKNOWN.
- **winsqlite3.h**: present in ALL installed SDKs incl. target version at `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\winsqlite\winsqlite3.h` (also 10.0.19041.0, 10.0.22621.0). P01-A probe had checked the wrong subdir (`ucrt`).
- **MakeAppx.exe**: `C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\makeappx.exe` (+ x86).
- **SignTool.exe**: `C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\arm\signtool.exe`, `\arm64\signtool.exe`.
- **Windows App SDK / WinUI 3 path**: NuGet cache has `microsoft.windowsappsdk` **2.4.0** at `%USERPROFILE%\.nuget\packages\microsoft.windowsappsdk` - restore path available, no provisioning needed.
- **winget**: `C:\Users\james\AppData\Local\Microsoft\WindowsApps\winget.exe`.
- **vs_installer**: `C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe` (available for component adds if ever needed).

## PROVISIONED (this microtask)

**vcpkg was the only missing first-party tool.** Provisioned noninteractively from the first-party Microsoft repo:

1. `git clone --depth 1 https://github.com/microsoft/vcpkg.git C:\vcpkg` - exit 0, cwd D:\GitHub\Forge-Conductor-Windows-Edition
2. `C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics` - downloaded prebuilt tool release, **signature validated**; result: `vcpkg package management program version 2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8`
3. First manifest install failed: shallow clone lacked pinned baseline `00c5775211f45cd08b37fce0484b4cb940e422ab`. Fixed with `git fetch --depth 1 origin 00c5775211f45cd08b37fce0484b4cb940e422ab` in C:\vcpkg (exit 0).
4. Focused validation - manifest-mode install from repo root using the repo's own `vcpkg.json` (name forge-conductor-windows, builtin-baseline 00c5775...):
   - Command: `set VCPKG_ROOT=C:\vcpkg && C:\vcpkg\vcpkg.exe install --triplet x64-windows`, cwd D:\GitHub\Forge-Conductor-Windows-Edition, **exit code 0**
   - Compiler detected: `C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe`
   - Installed: `nlohmann-json:x64-windows@3.12.0#2`, `vcpkg-cmake:x64-windows@2025-08-07`, `vcpkg-cmake-config:x64-windows@2026-07-21` (licenses: MIT)
   - "All requested installations completed successfully in: 35.9 ms"

## ACCEPTANCE MAPPING

- required source/governance evidence verified: AGENTS.md (vcpkg + nlohmann-json via Forsetti-approved vcpkg path), docs/WINDOWS_TOOLCHAIN.md ("Provision only first-party supported tooling noninteractively"), repo vcpkg.json - all read and applied.
- implementation complete: vcpkg provisioned; no other missing first-party tools found (SDK 26100, winsqlite3, MakeAppx, SignTool, Windows App SDK restore path all verified present).
- focused validation passes: manifest install of pinned nlohmann-json succeeded against MSVC 14.51.36231 / x64-windows (exit 0).
- evidence and handoff recorded: this file + probe.ps1 + probe-output.txt; session handoff follows.

## REMAINING LIMITATIONS / NEXT ACTIONS

- `.forge-qwen/state/microtasks.json` still shows P01-B `not_started` - next session must set status complete with this evidence before starting P01-C.
- vcpkg.exe binary hash not yet recorded (defer to P01-C "Record pinned versions and build environment").
- Build environments must export `VCPKG_ROOT=C:\vcpkg` for CMake manifest mode; record in P01-C.
- Implement agent session 28ec6de4-eddb-fec7-8004-41e8faa2397d may still be open - complete it from the next chat.
