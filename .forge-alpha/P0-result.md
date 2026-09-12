# Foundation result — September 12, 2026

Repository: `D:\GitHub\Forge-Conductor-Windows-Edition`, branch `alpha/native-desktop`,
base commit `14660648599378c85c3cdade5bd44ffe1cded079` plus working changes.

- Verified all 57 instruction-package SHA-256 entries before adoption. Reviewed overlapping AGENTS/README
  diffs and changes since audit commit; newer toolchain records were preserved.
- Adopted Alpha instructions and docs. Historical Qwen root selectors now refer to Alpha instructions.
  Existing dirty Qwen ledgers, evidence and temporary files were preserved.
- Untracked only generated root CMake output and two scratch binaries; their local files remain.
- Restored Forsetti from fixed upstream `63b9db87c575b2c72bfb6b3c988fcd7abd7fabe5`.
  Exact tree digest: `cfc377fee173cddc515f18f9c28db58e10b468402a72541a1f0ad524a1073c28`.
  Resolved the upstream template filename case collision to reproduce the original Windows archive.
  Existing mismatched input is refused. Build performs automatic verified restoration.
- Ran `scripts/build.ps1 -Configuration Debug -Architecture x64 -Target ForgeConductor.Cli,ForgeConductor.Manager,ForgeConductor.SessionHost -Parallel 4`.
  CMake 4.4.2, VS 2022/v143, Windows SDK 10.0.26100.0, `VCPKG_ROOT=C:\vcpkg`.
  Exit 0. All three native product targets built. Log: `out/alpha-evidence/p0-build.log`.
- Actual CLI filename is `forge-conductor.exe`. `--help` exits 0 and prints the supported commands.
- Quota discovery: `out/alpha-evidence/quota-discovery.log`. Active continuity candidates include
  `src/Application/ContinuityAutomation.cpp` and `include/ForgeConductor/Domain/ContinuityAutomationModels.h`.
- GitHub plan preview failed because GitHub CLI has no authentication. Log:
  `out/alpha-evidence/github-preview.log`. No remote writes or status changes occurred.
- Preflight falsely reported missing SQLite header; actual header is SDK `um/winsqlite/winsqlite3.h`.

Backend compilation is not installed-app or provider acceptance. P1 GUI and packaging remain in progress.
