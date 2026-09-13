# Windows toolchain

Discover actual Windows 11 build/architecture, PowerShell, Git, VS/Build Tools/MSVC, CMake/CTest, Windows SDK, Winsqlite3, MakeAppx, SignTool, vcpkg, and Windows App SDK. Provision only first-party supported tooling noninteractively. VS Code is editor, not compiler.

`cmake_minimum_required` in `CMakeLists.txt` is a **minimum** (3.28), not an equality pin.

## Pinned build environment (this host, 2026-09-11)

| Tool | Pin |
|---|---|
| CMake | **4.4.2** at `C:\Program Files\CMake\bin\cmake.exe` |
| Visual Studio | **Community 2026 18.9.0** at `C:\Program Files\Microsoft Visual Studio\18\Community` |
| MSVC | **14.51.36231** / `cl` 19.51.36256 x64 |
| Also installed | VS 2022 Community + Build Tools 17.14.27, toolset 14.44.35207 (fallback) |
| Windows SDK | **10.0.26100.0** at `C:\Program Files (x86)\Windows Kits\10` |
| Git | 2.53.0 |
| Windows App SDK | 2.4.0 (`%USERPROFILE%\.nuget\packages\microsoft.windowsappsdk\2.4.0`) |

Machine-readable copy: `.forge-qwen/state/toolchain-pins.json`.

## vcpkg (canonical root)

**`VCPKG_ROOT` must be `C:\vcpkg`.** That is the P01-B clone used for the repo manifest (`builtin-baseline` `00c5775211f45cd08b37fce0484b4cb940e422ab`).

| | |
|---|---|
| Root | `C:\vcpkg` |
| Toolchain file | `C:\vcpkg\scripts\buildsystems\vcpkg.cmake` |
| Version | `2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8` |
| `vcpkg.exe` SHA256 | `13B8175E99A884C5AD34249218754B45541A1A63F216E92603AEE57A285AC741` |

`CMakePresets.json` consumes `$env{VCPKG_ROOT}`. Do not use `C:\Users\james\AppData\Local\ForgeConductor\Toolchain\vcpkg` for this repository even if that copy’s `vcpkg.exe` hash matches; it is the Forge installer’s toolchain, not the P01-B baseline clone.

Manifest mode from repo root: `nlohmann-json:x64-windows@3.12.0#2`.

## Discovery

`where cl` is not an inventory. Use `vswhere` (on PATH) or Microsoft’s locator under `Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe`. SDK headers live under **`(x86)\Windows Kits\10`**, not `C:\Program Files\Windows Kits\10`.

<!-- alpha-phase-review:start -->
Phase review: R6 continuation — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
