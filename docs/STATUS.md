# Product status

Updated September 14, 2026 for Forge Conductor 1.1.

Forge Conductor is implemented as a native Windows 11 x64 product with a complete WinUI 3 GUI and production runtime. The source builds all four shipping executables: `ForgeConductorApp.exe`, `forge-conductor.exe`, `ForgeConductor.Manager.exe`, and `ForgeConductor.SessionHost.exe`.

| Area | Current state |
|---|---|
| GUI | Rig, Autonomy, Continuity, Projects, Memory, Tools, and Settings are implemented with keyboard, High Contrast, and scaled-text coverage. |
| Local-model execution | Manager-owned LM Studio Responses runs, tool calls, rollover, reattachment, pause/resume/stop, and retained-context reporting are implemented. |
| Data | Production profile is `%LOCALAPPDATA%\Forge Conductor`; central schemas 3, 5, 6, 7, 8, and released 9 are strictly validated and migrated or opened without silent mutation. |
| Projects and tools | Stable project identities, alias isolation, legacy/project memory, and 57 native tools are exposed through authenticated Manager and MCP paths. |
| LM Studio MCP | Transactional primary, fallback, and dedicated four-tool CLU registrations share one revision and report role-specific health. Shared MCP control calls fail closed when native task identity is unavailable. |
| Telemetry | Native CPU, RAM, GPU-engine/memory, disk/volume, process, workflow, provider, store, and continuity observations are presented in the GUI. |
| Packaging | Stable package identity `ForgeConductor.Windows`, signed MSIX, verified payload manifest, public certificate, installer helper, optional App Installer updates, and provenance metadata are implemented. |
| Automation | Windows Release CI and a secret-backed production-signing workflow are checked in. |

The current verification baseline is a successful complete x64 Release product build. The suite contains 150 CTest entries; final package evidence is generated only from a clean committed tree because packaging intentionally rejects dirty product inputs.

Historical 0.9.x and 1.0 candidate records document prior validation only and are not 1.1 release artifacts.
