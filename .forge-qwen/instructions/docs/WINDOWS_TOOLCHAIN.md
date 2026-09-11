# Windows toolchain

Discover actual Windows 11 build/architecture/RAM/GPU/disk, PowerShell, Git, VS/Build Tools/MSVC, CMake/CTest, Windows SDK, Winsqlite3, MakeAppx, SignTool, WPR/WPA, WinDbg/CDB, AppVerifier/GFlags, vcpkg, Windows App SDK restore, Sandbox/VM/ARM64 execution, LM Studio API/config, and signing certificates.

Provision only first-party supported tooling noninteractively. VS Code is editor, not compiler. Pin resolved versions after discovery. Missing privilege/network/tool blocks corresponding gates but not independent source/architecture work.

## Pinned build environment

See repo `docs/WINDOWS_TOOLCHAIN.md` and `.forge-qwen/state/toolchain-pins.json`.

- CMake **4.4.2** (minimum in CMakeLists is 3.28)
- VS **Community 2026 18.9** / MSVC **14.51.36231**
- Windows SDK **10.0.26100.0** at `C:\Program Files (x86)\Windows Kits\10`
- **`VCPKG_ROOT=C:\vcpkg`** only (not the Forge AppData toolchain copy)

