# Product status

Updated September 23, 2026 for the Forge Conductor 1.1.44 engineering prerelease.

Version 1.1.44 adds 20 searchable offline setup/help articles, corrects the opening folder-picker action, and isolates lifecycle tests from the live Manager port. See [release notes](releases/1.1.44.md), [setup/governance research](SETUP-GOVERNANCE-RESEARCH.md), and [verification limits](validation/SETUP-RESEARCH-2026-09-23.md). Policy repository import and enforcement are not implemented. Full installed acceptance remains open; this candidate must not be described as fully shippable. Stale Manager detail cards after disconnect and a lingering isolated Manager after a stop request are recorded for follow-up.

Forge Conductor is implemented as a native Windows 11 x64 product with a complete WinUI 3 GUI and production runtime. The source builds all four shipping executables: `ForgeConductorApp.exe`, `forge-conductor.exe`, `ForgeConductor.Manager.exe`, and `ForgeConductor.SessionHost.exe`.

| Area | Current state |
|---|---|
| GUI | Fifteen native destinations are implemented. Version 1.1.43 adds a dedicated Guided setup destination and a persistent nine-step path covering real folder authorization, Manager verification, optional instructions and memory, provider readiness, and the first managed task. Production-data, final exact-byte scaling, High Contrast, and keyboard-only installed acceptance remain open. |
| Local-model execution | Manager-owned LM Studio Responses runs, tool calls, rollover, reattachment, pause/resume/stop, and retained-context reporting are implemented. |
| Data | Production profile is `%LOCALAPPDATA%\Forge Conductor`; central schemas 3, 5, 6, 7, 8, released 9 and 10 migrate through immutable history to nullable audit-project schema 11. Historical audit rows remain unscoped. |
| Projects and tools | Stable project identities, alias isolation, legacy/project memory, checksummed project instruction packages, and 57 native tools are exposed through authenticated Manager and MCP paths. Package validation is read-only; activation persists file records and an exact active manifest, and new runs receive the active revision within the bounded task budget. |
| LM Studio MCP | Transactional primary, fallback, and dedicated four-tool CLU registrations share one revision and report role-specific health. Installed 1.1.35 CLI and LM Studio show the four CLU tools; an authorized desktop-source start still returns `task_identity_unavailable` without a native source binding. R22 is not qualified. |
| Telemetry | Native CPU, RAM, GPU-engine/memory, disk/volume, process, workflow, provider, store, and continuity observations are presented in the GUI. |
| Packaging | Stable package identity `ForgeConductor.Windows`, signed MSIX, verified payload manifest, public certificate, installer helper, optional App Installer updates, and provenance metadata are implemented. |
| Automation | Windows Release CI and a secret-backed production-signing workflow are checked in. |

Version 1.1.43 expands guided setup across the existing Manager-owned project registration, instruction-package, durable-memory, provider, and managed-run paths. The guide does not synthesize project data and advances authoritative actions only after their Manager readback. Exact-commit Release, signed installation, every-view screenshots, and installed package interaction are required before its release record is complete. The live model-only response is not native task verification. Desktop-source handoff, useful successor work, production and final accessibility proofs remain open. Packaging intentionally rejects dirty product inputs.

Historical 0.9.x and 1.0 candidate records document prior validation only and are not 1.1 release artifacts.
