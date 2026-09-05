# P01-C: Record pinned versions and build environment - Facts

## VERIFIED (this microtask, session 48915a42)

### vcpkg (provisioned in P01-B at C:\vcpkg)
- Version string: `vcpkg package management program version 2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8` (re-confirmed via `C:\vcpkg\vcpkg.exe version`, exit 0)
- **Binary hash: SHA256 of C:\vcpkg\vcpkg.exe = `13B8175E99A884C5AD34249218754B45541A1A63F216E92603AEE57A285AC741`** (computed this session with Get-FileHash; was the one P01-B deferral)
- Provisioning provenance: `git clone --depth 1 https://github.com/microsoft/vcpkg.git C:\vcpkg` + `bootstrap-vcpkg.bat -disableMetrics` (prebuilt tool release, signature validated); shallow clone then fetched pinned baseline commit.

### Manifest pin (repo vcpkg.json, read this session)
- name: `forge-conductor-windows`, version-string `0.1.0`
- **builtin-baseline: `00c5775211f45cd08b37fce0484b4cb940e422ab`** (vcpkg registry commit all versions resolve against)
- dependencies: `nlohmann-json`

### Installed packages (x64-windows triplet, manifest mode from repo root)
- `nlohmann-json:x64-windows@3.12.0#2` (MIT) - the only direct dependency
- Tool deps pulled by vcpkg itself: `vcpkg-cmake:x64-windows@2025-08-07`, `vcpkg-cmake-config:x64-windows@2026-07-21`

### Compiler / toolchain (detected by vcpkg this session)
- MSVC: `C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe` -> VS Community channel "18", toolset **14.51.36231**
- Windows SDK: **10.0.26100.0** (include dir `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0`; winsqlite3.h at `...\um\winsqlite\winsqlite3.h`)
- MakeAppx.exe / SignTool.exe: from SDK bin dir `10.0.19041.0` (per P01-B probe)
- CMake 3.28, Windows App SDK **2.4.0** (NuGet cache `%USERPROFILE%\.nuget\packages\microsoft.windowsappsdk`) - per P01-A/P01-B probes

### Environment requirement for builds
- Build environments MUST export `VCPKG_ROOT=C:\vcpkg` so CMake manifest mode finds the provisioned vcpkg (recorded in docs/WINDOWS_TOOLCHAIN.md this session).

## FOCUSED VALIDATION (this microtask)
- Command: `$env:VCPKG_ROOT='C:\vcpkg'; C:\vcpkg\vcpkg.exe install --triplet x64-windows`, cwd `D:\GitHub\Forge-Conductor-Windows-Edition` -> **exit 0**
- Output: "The following packages are already installed: nlohmann-json:x64-windows@3.12.0#2 ... All requested installations completed successfully in: 159 us" (idempotent; proves pin reproducibility)

## ACCEPTANCE MAPPING
- required source/governance evidence verified: repo `vcpkg.json` (baseline + dep), P01-B FACTS.md (provenance, SDK/MSVC/AppSDK facts), docs/WINDOWS_TOOLCHAIN.md (first-party-only provisioning rule).
- implementation complete: pinned versions + build environment recorded in this file, `.forge-qwen/state/toolchain-pins.json` (machine-readable), and `docs/WINDOWS_TOOLCHAIN.md` ("Pinned build environment" section); P01-B state closed with evidence pointer.
- focused validation passes: idempotent manifest install exit 0 against MSVC 14.51.36231 / x64-windows; vcpkg version + binary hash captured.
- evidence and handoff recorded: this file + toolchain-pins.json + doc update; session handoff follows.

## REMAINING LIMITATIONS / NEXT ACTIONS
- No source code exists yet (repo is docs/state only) - P01-D "Create minimal CMake project skeleton" is the natural next microtask, and can now rely on these pins.
- vcpkg binary hash recorded here; if vcpkg is ever re-bootstrapped, compare SHA256 against `13B8175E99A884C5AD34249218754B45541A1A63F216E92603AEE57A285AC741`.
