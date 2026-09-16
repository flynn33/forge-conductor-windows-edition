# Product status

Updated September 15, 2026 for the Forge Conductor 1.1.35 engineering candidate.

Forge Conductor is implemented as a native Windows 11 x64 product with a complete WinUI 3 GUI and production runtime. The source builds all four shipping executables: `ForgeConductorApp.exe`, `forge-conductor.exe`, `ForgeConductor.Manager.exe`, and `ForgeConductor.SessionHost.exe`.

| Area | Current state |
|---|---|
| GUI | Fourteen native destinations are implemented. The signed 1.1.34 archive-profile visual sweep exists; production-data, 100/150/200% text, keyboard-only and High Contrast installed acceptance are still open. |
| Local-model execution | Manager-owned LM Studio Responses runs, tool calls, rollover, reattachment, pause/resume/stop, and retained-context reporting are implemented. |
| Data | Production profile is `%LOCALAPPDATA%\Forge Conductor`; central schemas 3, 5, 6, 7, 8, released 9 and 10 migrate through immutable history to nullable audit-project schema 11. Historical audit rows remain unscoped. |
| Projects and tools | Stable project identities, alias isolation, legacy/project memory, and 57 native tools are exposed through authenticated Manager and MCP paths. |
| LM Studio MCP | Transactional primary, fallback, and dedicated four-tool CLU registrations share one revision and report role-specific health. Installed 1.1.34 CLI and LM Studio show the four CLU tools; a start request returns `task_identity_unavailable` without a native desktop-source binding. R22 is not qualified. |
| Telemetry | Native CPU, RAM, GPU-engine/memory, disk/volume, process, workflow, provider, store, and continuity observations are presented in the GUI. |
| Packaging | Stable package identity `ForgeConductor.Windows`, signed MSIX, verified payload manifest, public certificate, installer helper, optional App Installer updates, and provenance metadata are implemented. |
| Automation | Windows Release CI and a secret-backed production-signing workflow are checked in. |

Committed 1.1.34 passed the complete x64 Release build and 151/151 CTest entries and produced a signed installed MSIX. The 1.1.35 source candidate passed the documented x64 Release Product All build, 151/151 tests, native-stack/no-attribution static gates and package-persistence checks after the authoritative-project audit and Direction A shell retheme. A disposable staged-app read-only invocation and exact-project Feed filter also succeeded. Exact-commit packaging, signed install and complete interaction evidence remain pending. Packaging intentionally rejects dirty product inputs.

Historical 0.9.x and 1.0 candidate records document prior validation only and are not 1.1 release artifacts.
